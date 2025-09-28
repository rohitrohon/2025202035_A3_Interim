CC = g++
CFLAGS = -Wall -std=c++17 -pthread -I/opt/homebrew/opt/openssl@3/include -g
LDFLAGS = -pthread -L/opt/homebrew/opt/openssl@3/lib -lcrypto -lssl

# Source files
SRCS = tracker.cpp \
       client_handler.cpp \
       data_structures.cpp \
       synchronization.cpp

# Object files
OBJS = $(SRCS:.cpp=.o)

# Header files
DEPS = $(wildcard *.h)

# Output binary
TARGET = tracker

# Default target
all: $(TARGET)

# Link object files to create the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files to object files
%.o: %.cpp $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)

# Run the tracker with default arguments
run1: all
	./$(TARGET) tracker_info.txt 1

run2: all
	./$(TARGET) tracker_info.txt 2

# Phony targets (don't represent actual files)
.PHONY: all clean run1 run2