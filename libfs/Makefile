# Target library
lib := libfs.a
obj := disk.o fs.o
CC := gcc
CFLAGS := -Wall -Wextra -Werror

## Debug flag
ifneq ($(D),1)
CFLAGS  += -O2
else
CFLAGS  += -g
endif

ifneq ($(V),1)
Q = @
endif

all: $(lib)

deps := $(patsubst %.o,%.d,$(obj))
-include $(deps)
DEPFLAGS = -MMD -MF $(@:.o=.d)

$(lib): $(obj) 
	ar rcs $(lib) $(obj) 

%.o: %.c
	$(Q) $(CC) $(CFLAGS) -c -o $@ $< $(DEPFLAGS)

clean:
	$(Q)rm -f $(lib) $(obj) $(deps)

# Adapted from Joel Porquet lecture slides

## TODO: Phase 1
