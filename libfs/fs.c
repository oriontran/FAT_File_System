#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define FAT_EOC 0xFFFF
#define FAT_MAX_ENTRIES 2048
#define SUPERBLOCK_LOC 0
#define FAT_BLOCK_LOC 1
#define FD_MIN 0

int free_fat; 
int free_root;
int open_files;

unsigned int fd_count;

struct FS *FS;

enum OP {
	SMALL,
	IDEAL,
	BIG,
};

static enum OP OP_type(size_t count, size_t offset) {
	if (offset == 0 && count == BLOCK_SIZE )
		return IDEAL;
	if (offset == 0 && count < BLOCK_SIZE)
		return SMALL;
	if (offset > 0 && count < BLOCK_SIZE - (offset % BLOCK_SIZE)) 
		return SMALL;
	return BIG;
}


struct __attribute__((packed)) Root_Entry 
{
	char file_name[16];
	uint32_t file_size;
	uint16_t first_d_index;
	uint8_t padding[10];
};

struct FD {
	struct Root_Entry* file_ptr;
	unsigned int offset;
};

struct FD* fd_table[32];
int fd_initialized = 0;

static void fd_initialize(void) 
{
	for (size_t i = 0; i < sizeof(fd_table)/sizeof(fd_table[0]); i++) {
		fd_table[i] = NULL;
	}
}

struct __attribute__((packed)) FAT 
{
	uint16_t num_blocks[2048];
	struct FAT* fat_next;	
};

struct __attribute__((packed)) SuperBlock 
{
	uint64_t sig;
	uint16_t disk_blocks;
	uint16_t root_index;
	uint16_t data_index;
	uint16_t data_block_num;
	uint8_t FAT_block_num;
	uint8_t pad[4079];
};

struct FS 
{
	struct SuperBlock* super;
	struct FAT* fat;
	struct Root_Entry* root;
};

int fs_mount(const char *diskname)
{
	if (block_disk_open(diskname) == -1) {
		return -1;
	}
	
	struct SuperBlock* SuperBlock = malloc(sizeof(struct SuperBlock));
	block_read(SUPERBLOCK_LOC, (void *)SuperBlock);

	if (memcmp((void *)&SuperBlock->sig, "ECS150FS", sizeof(uint64_t))) {

		return -1;
	}

	if (SuperBlock->disk_blocks != block_disk_count()) {
		return -1;
	}

	struct FAT* FAT = malloc(sizeof(struct FAT));
	struct FAT* fat_ptr = FAT;
	for (int i = 0; i < SuperBlock->FAT_block_num; i++) {
		block_read(FAT_BLOCK_LOC + i, (void *)fat_ptr);
		if (i < SuperBlock->FAT_block_num - 1) {
			fat_ptr->fat_next = malloc(sizeof(struct FAT));
			fat_ptr = fat_ptr->fat_next;	
		} else {
			fat_ptr->fat_next = NULL;
		}
	}

	struct Root_Entry* root = calloc(FS_FILE_MAX_COUNT, sizeof(struct Root_Entry));
	block_read(SuperBlock->root_index, (void *)root);

	FS = malloc(sizeof(struct FS));
	FS->super = SuperBlock;
	FS->fat = FAT;
	FS->root = root;
	int count = FS->super->data_block_num;
	
	struct FAT *fat_ptr2 = FS->fat;
	for (int i = 0; i < FS->super->FAT_block_num; i++) {
		if (count >= FAT_MAX_ENTRIES) {
			for (int h = 0; h < FAT_MAX_ENTRIES; h++) {
				if (fat_ptr2->num_blocks[h] == 0)
					free_fat++;
			}
			count -= FAT_MAX_ENTRIES;
		} else {
			for (int h = 0; h < count; h++) {
				if (fat_ptr2->num_blocks[h] == 0)
					free_fat++;
			}
		}

		fat_ptr2 = fat_ptr2->fat_next;
	}
	for (int j = 0; j < FS_FILE_MAX_COUNT; j++) {
		if (!strcmp(FS->root[j].file_name, ""))
			free_root++;
	}
	return 0;
}

int fs_umount(void)
{
	if (!FS) 
		return -1;
	block_write(SUPERBLOCK_LOC, FS->super);
	struct FAT *fat_ptr = FS->fat;
	struct FAT *fat_ptr2;
	for (int i = 0; i < FS->super->FAT_block_num; i++) {
		block_write(FAT_BLOCK_LOC + i, fat_ptr->num_blocks);
		fat_ptr = fat_ptr->fat_next;
	}
	block_write(FS->super->root_index, FS->root);
	fat_ptr = FS->fat;
	for (int i = 0; i < FS->super->FAT_block_num; i++) {
		fat_ptr2 = fat_ptr->fat_next;
		free(fat_ptr);
		fat_ptr = fat_ptr2;
	}
	free(FS->root);
	free(FS->super);
	free(FS);
	if (block_disk_close() == -1)
		return -1;
	return 0;
}

int fs_info(void)
{
	fprintf(stdout, "FS Info:\n");
	fprintf(stdout, "total_blk_count=%u\n", (unsigned int)FS->super->disk_blocks);
	fprintf(stdout, "fat_blk_count=%d\n", FS->super->FAT_block_num);
	fprintf(stdout, "rdir_blk=%u\n", (unsigned int)FS->super->root_index);
	fprintf(stdout, "data_blk=%u\n", (unsigned int)FS->super->data_index);
	fprintf(stdout, "data_blk_count=%u\n", (unsigned int)FS->super->data_block_num);
	fprintf(stdout, "fat_free_ratio=%d/%u\n", free_fat, FS->super->data_block_num);
	fprintf(stdout, "rdir_free_ratio=%d/%u\n", free_root, FS_FILE_MAX_COUNT);

	return 0;
}

int fs_create(const char *filename)
{
	if (!filename || strlen(filename) > FS_FILENAME_LEN)
		return -1;
	
	struct Root_Entry *root = FS->root;
	struct FAT* fat_ptr = FS->fat;
	int count = FS->super->data_block_num;
	int iterations = 0;

	for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (!strcmp(root[i].file_name, filename))
			return -1;
	}

	root = FS->root;
	for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (!strcmp(root[i].file_name, "")) {
			memcpy(root[i].file_name, filename, strlen(filename));
			root[i].file_size = 0;
			for (int j = 0; j < FS->super->FAT_block_num; j++) {
				if (count > FAT_MAX_ENTRIES) {
					count -= FAT_MAX_ENTRIES;
					iterations = FAT_MAX_ENTRIES;
				} else {
					iterations = count;
				}
				for (int h = 0; h < iterations; h++) {
					if (free_fat == 0) { 
						root[i].first_d_index = FAT_EOC;
						free_root--;
						return 0;
					}
					if (FS->fat->num_blocks[h] == 0) {
						FS->fat->num_blocks[h] = FAT_EOC;
						free_fat--; 
						root[i].first_d_index = h;
						free_root--;
						return 0;
					} 
				}
				fat_ptr = fat_ptr->fat_next;
				break;
			} 
		}
	}
	return -1;
}

int fs_delete(const char *filename)
{
	if (!filename || strlen(filename) > FS_FILENAME_LEN)
		return -1;

	struct Root_Entry* root = FS->root;
	struct FAT* fat_ptr = FS->fat;

	for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {

		if (!strcmp(root[i].file_name, filename)) {

			free_root++;
			int marker = fat_ptr->num_blocks[root[i].first_d_index];
			fat_ptr->num_blocks[root[i].first_d_index] = 0;
			int index;
			if (marker == FAT_EOC) {
				memset((void *)&fat_ptr->num_blocks[root[i].first_d_index], 0, sizeof(uint16_t));
				memset(root[i].file_name, '\0', sizeof(root[i].file_name));
				free_fat++;
				return 0;
			}
			while (fat_ptr->num_blocks[marker] != FAT_EOC) {
				index = fat_ptr->num_blocks[marker];
				memset((void *)&fat_ptr->num_blocks[marker], 0, sizeof(uint16_t));
				
				free_fat++;
				marker = index;
			}
			memset((void *)&fat_ptr->num_blocks[marker], 0, sizeof(uint16_t));
			memset(root[i].file_name, '\0', sizeof(root[i].file_name));
			return 0;
		}
	} 
	return -1;
}

int fs_ls(void)
{

	if (!FS)
		return -1;

	struct Root_Entry* root = FS->root;

	printf("FS Ls:\n");

	for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (root[i].file_name[0])
			printf("file: %s, size: %d, data_blk: %d\n", root[i].file_name, root[i].file_size, root[i].first_d_index);
	}

	return 0;
}

int fs_open(const char *filename)
{
	if (fd_count >= FS_OPEN_MAX_COUNT)
		return -1;

	struct Root_Entry* root = FS->root;
	struct FD* new_fd = malloc(sizeof(struct FD));
	for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (!strcmp(root[i].file_name, filename)) {	
			new_fd->file_ptr = &root[i];
			new_fd->offset = 0;
			fd_count++;
			break;
		} else if (strcmp(root[i].file_name, filename) && i == FS_FILE_MAX_COUNT -1) {
			free(new_fd);
			return -1;
		}
	}
	for (int j = 0; j < FS_OPEN_MAX_COUNT; j++) {
		if (!fd_initialized) {
			fd_initialize();
			fd_initialized++;
		}
		if (fd_table[j] == NULL) { 
			fd_table[j] = new_fd;
			return j;
		}
	}

	return -1;
}

int fs_close(int fd)
{
	if (fd > FS_OPEN_MAX_COUNT - 1 || fd < FD_MIN || fd_table[fd] == NULL)
		return - 1;

	free(fd_table[fd]);
	fd_table[fd] = NULL;
	fd_count--;
	return 0;
}

int fs_stat(int fd)
{
	if (fd >= FS_OPEN_MAX_COUNT || fd < 0 || fd_table[fd] == NULL)
		return -1;

	return fd_table[fd]->file_ptr->file_size;
}

int fs_lseek(int fd, size_t offset)
{
	if (fd >= FS_OPEN_MAX_COUNT || fd < 0 || offset > fd_table[fd]->file_ptr->file_size)
		return -1;

	fd_table[fd]->offset = offset;

	return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
	if (fd > FS_OPEN_MAX_COUNT - 1 || fd < FD_MIN || fd_table[fd] == NULL)
		return - 1;

	char* bounce = malloc(BLOCK_SIZE);
	int fat_index = fd_table[fd]->file_ptr->first_d_index;
	unsigned int offset;

	if (fd_table[fd]->file_ptr->first_d_index == FAT_EOC)
		return 0;

	int bytes_left;
	int bytes_written = 0;
	int block_count = 1;
	int number = FS->super->data_block_num;
	int forloop; 
	int forloopflag = 0;
	int offset2 = fd_table[fd]->offset;
	int marker1 = 0;

	int fat_count;
	struct FAT *fat_ptr = FS->fat;

	enum OP operation = OP_type(count, fd_table[fd]->offset);

	switch(operation) 
	{
		case SMALL:
			offset = fd_table[fd]->offset;
			block_read(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
			if (count + offset > BLOCK_SIZE) {
				memcpy(bounce + offset, buf, BLOCK_SIZE - offset);
				block_write(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
				memset(bounce, '\0', BLOCK_SIZE);
				bytes_written += BLOCK_SIZE - offset;
				for (int i = 0; i < FS->super->FAT_block_num; i++) {
					if (number >= FAT_MAX_ENTRIES) {
						number -= FAT_MAX_ENTRIES;
						forloop = FAT_MAX_ENTRIES;
					} else {
						forloop = number;
					}
					for (int j = 0; j < forloop; j++) {
						if (FS->fat->num_blocks[j] == 0) {
							fat_index = j;
							forloopflag = 1;
							free_fat--;
						}
						break;
					}
					if (forloopflag)
						break;
				}
				if (forloopflag == 0)
					return 0;

				memcpy(bounce, buf + bytes_written, count - bytes_written);
				block_write(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
				offset += count - bytes_written;
			} else {
				memcpy(bounce + offset, buf, count); 
				block_write(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
				bytes_written += count;
				offset += count;
			}
			fd_table[fd]->offset = offset;
			break;
		case IDEAL:
			offset = fd_table[fd]->offset;
			block_write(fat_index + FS->super->FAT_block_num + 2, (void*)buf);
			bytes_written += BLOCK_SIZE;
			offset += BLOCK_SIZE;
			fd_table[fd]->offset = offset;
			break;
		default:
			offset = fd_table[fd]->offset;
			bytes_left = count; 

			while (bytes_left > 0) {
				fat_count = FS->super->data_block_num;
				if (block_count == 1) {
					block_read(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
					memcpy(bounce + offset % BLOCK_SIZE, buf, BLOCK_SIZE - (offset % BLOCK_SIZE));
					block_write(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
					bytes_left -= (BLOCK_SIZE - (offset % BLOCK_SIZE));
					bytes_written += (BLOCK_SIZE - (offset % BLOCK_SIZE));

					if (FS->fat->num_blocks[fat_index] == FAT_EOC) {
						for (int i = 0; i < FS->super->FAT_block_num; i++) {
							if (fat_count > FAT_MAX_ENTRIES) {
								for (int k = 0; k < FAT_MAX_ENTRIES; k++) {
									if (fat_ptr->num_blocks[k] == 0) {
										FS->fat->num_blocks[fat_index] = k;
										fat_index = k;
										FS->fat->num_blocks[k] = FAT_EOC;
										free_fat--;
										marker1 = 1;
										break;
									}
								} 
								fat_count -= FAT_MAX_ENTRIES;
							} else { 
								for (int j = 0; j < fat_count; j++) {
									if (fat_ptr->num_blocks[j] == 0) {
										FS->fat->num_blocks[fat_index] = j;
										fat_index = j;
										FS->fat->num_blocks[j] = FAT_EOC;
										free_fat--;
										marker1 = 1;
										break;
									}
								}
							}
							if (marker1) 
								break;
						}
						if (marker1 == 0) {
							offset += bytes_written;
							fd_table[fd]->offset = offset;
							fd_table[fd]->file_ptr->file_size += bytes_written - (fd_table[fd]->file_ptr->file_size - offset2);
							return bytes_written;
						}
					}
					offset += bytes_written;
					block_count++;
				} else if (bytes_left > BLOCK_SIZE) {
					block_read(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
					memcpy(bounce, buf + bytes_written, BLOCK_SIZE);
					block_write(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
					bytes_left -= BLOCK_SIZE;
					bytes_written += BLOCK_SIZE;
					if (FS->fat->num_blocks[fat_index] == FAT_EOC) {
						for (int i = 0; i < FS->super->FAT_block_num; i++) {
							if (fat_count > FAT_MAX_ENTRIES) {
								for (int k = 0; k < FAT_MAX_ENTRIES; k++) {
									if (fat_ptr->num_blocks[k] == 0) {
										FS->fat->num_blocks[fat_index] = k;
										fat_index = k;
										FS->fat->num_blocks[k] = FAT_EOC;
										free_fat--;
										marker1 = 1;
										break;
									}
								} 
								fat_count -= FAT_MAX_ENTRIES;
							} else { 
								for (int j = 0; j < fat_count; j++) {
									if (fat_ptr->num_blocks[j] == 0) {
										FS->fat->num_blocks[fat_index] = j;
										fat_index = j;
										FS->fat->num_blocks[j] = FAT_EOC;
										free_fat--;
										marker1 = 1;
										break;
									}
								}
							}
							if (marker1)
								break;
						}
						if (marker1 == 0) {
							offset += BLOCK_SIZE;
							fd_table[fd]->offset = offset;
							fd_table[fd]->file_ptr->file_size += bytes_written - (fd_table[fd]->file_ptr->file_size - offset2);
							return bytes_written;
						}
					}
					offset += BLOCK_SIZE;
				} else {
					block_read(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
					memcpy(bounce, buf + bytes_written, bytes_left);
					block_write(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
					bytes_written += bytes_left;
					offset += bytes_left;
					bytes_left -= bytes_left;
					FS->fat->num_blocks[fat_index] = FAT_EOC;
				}
			}
			fd_table[fd]->offset = offset;
			break;
	}
	fd_table[fd]->file_ptr->file_size += bytes_written - (fd_table[fd]->file_ptr->file_size - offset2);
	return bytes_written;
}

int fs_read(int fd, void *buf, size_t count)
{
	if (fd > FS_OPEN_MAX_COUNT - 1 || fd < FD_MIN || fd_table[fd] == NULL)
		return - 1;
	char* bounce = malloc(BLOCK_SIZE);
	char* temp;
	int fat_index = fd_table[fd]->file_ptr->first_d_index;
	unsigned int offset;
	uint32_t size;

	int bytes_left;
	int bytes_read = 0;
	int block_count = 1;
	int offset2 = fd_table[fd]->offset;

	enum OP operation = OP_type(count, fd_table[fd]->offset);

	switch(operation)
	{
		case SMALL:
			offset = fd_table[fd]->offset;
			size = fd_table[fd]->file_ptr->file_size;

			block_read(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);

			temp = bounce + offset;

			if (size == offset + 1)
				return 0;

			if (count + offset > size) {
				memcpy(buf, temp, size - offset);
				bytes_read += (size - offset);

				offset += (size - offset);
				fd_table[fd]->offset = offset;
			} else {
				memcpy(buf, temp, count);
				bytes_read += count;
				offset += count;
				fd_table[fd]->offset = offset;
			}
			break;
		case IDEAL:
			offset = fd_table[fd]->offset;
			size = fd_table[fd]->file_ptr->file_size;
			block_read(fat_index + FS->super->FAT_block_num + 2, buf);
			offset += BLOCK_SIZE;
			bytes_read += BLOCK_SIZE;
			fd_table[fd]->offset = offset;
			break;
		default:
			size = fd_table[fd]->file_ptr->file_size;
			offset = fd_table[fd]->offset;

			if (offset + count > size) {
				bytes_left = size - offset;
			} else {
				bytes_left = count;
			}

			while (bytes_left > 0) {
				if (block_count == 1) {
					temp = bounce +  offset % BLOCK_SIZE;
					block_read(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);

					if (size < BLOCK_SIZE) {
						memcpy(buf, temp, size - offset);
						bytes_left -= (size - offset);
						bytes_read += (size - offset);
						offset += (size - offset);
						fd_table[fd]->offset = offset;
					} else {
						memcpy(buf, temp, BLOCK_SIZE - (offset % BLOCK_SIZE));

						bytes_left -= (BLOCK_SIZE - (offset % BLOCK_SIZE));
						bytes_read += (BLOCK_SIZE - (offset % BLOCK_SIZE));
						offset += (BLOCK_SIZE - (offset % BLOCK_SIZE));
						fd_table[fd]->offset = offset;
					}
					fat_index = FS->fat->num_blocks[fat_index]; 
					block_count++;
				} else if (bytes_left <= BLOCK_SIZE) {
					block_read(fat_index + FS->super->FAT_block_num + 2, (void*)bounce);
					memcpy(buf + bytes_read, bounce, bytes_left);
					bytes_read += bytes_left;
					bytes_left -= bytes_left;
					offset += bytes_left;
					fd_table[fd]->offset = offset;
				} else {
					block_read(fat_index + FS->super->FAT_block_num + 2, bounce);

					if (size - bytes_read < BLOCK_SIZE) {
						memcpy(buf + bytes_read, bounce, size - bytes_read);
						bytes_left -= size - bytes_read;
						offset += (size - bytes_read);
						fd_table[fd]->offset = offset;
						bytes_read += (size - bytes_read);
					} else {
						memcpy(buf + bytes_read, bounce, BLOCK_SIZE);
						bytes_left -= BLOCK_SIZE;
						offset += BLOCK_SIZE;
						fd_table[fd]->offset = offset;
						bytes_read += BLOCK_SIZE;
					}
					fat_index = FS->fat->num_blocks[fat_index];
				}
			}
			break;
	}
	fd_table[fd]->file_ptr->file_size += bytes_read - (fd_table[fd]->file_ptr->file_size - offset2);
	return bytes_read;
}
