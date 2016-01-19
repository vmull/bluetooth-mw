TARGET = init

SRCS = \
	main.cc \
	bluez/lib/hci.c \
	bluez/lib/bluetooth.c \
	hci_worker.cc

#
# the pthread stuff is needed to make threading work because
# this toolchain doesn't link those in during static linking.
#
LIBS = -lm -L/SourceCache/nanox/microwindows-0.92/src/lib/ -lmwin -pthread \
	-Wl,-u,pthread_join,-u,pthread_equal,-u,pthread_detach

CC = arm-linux-gnueabi-gcc
CFLAGS = -g -Wno-pmf-conversions -pthread -std=c++11 -I/SourceCache/nanox/microwindows-0.92/src/include/ -I./bluez/lib/

CXXFLAGS = $(CFLAGS)
CXX = arm-linux-gnueabi-g++

.PHONY: default all clean

default: $(TARGET)
all:
	default

BUILD_DIR = build

OBJ := $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(SRCS))))
HEADERS := $(shell find . -type f -name '*.h*')

create_build_directory:
	@mkdir -p $(BUILD_DIR)

CREATE_SUBDIR = \
	@DIR="$(dir $@)"; \
	if [ ! -d $$DIR ]; then mkdir -p $$DIR; fi

$(BUILD_DIR)/%.o: %.cc $(HEADERS)
	$(CREATE_SUBDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c $(HEADERS)
	$(CREATE_SUBDIR)
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJ)

$(TARGET): create_build_directory $(OBJ)
	$(CXX) $(OBJ) -static -Wall $(LIBS) -o $@
	cp $(TARGET) /home/k/share/$(TARGET)

clean:
	-rm -rf build/
