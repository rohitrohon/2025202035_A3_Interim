# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -std=c++17 -pthread -I/opt/homebrew/opt/openssl@3/include -g
LDFLAGS = -pthread -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto

# Source files for client
CLIENT_SRCS = client.cpp \
             file_operations.cpp \
             network_utils.cpp

# Object files
CLIENT_OBJS = $(CLIENT_SRCS:.cpp=.o)

# Header files
DEPS = $(wildcard *.h)

# Output binary
TARGET = client

# Default target
all: $(TARGET)

# Link object files to create the executable
$(TARGET): $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files to object files
%.o: %.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Clean build artifacts
clean:
	rm -f $(CLIENT_OBJS) $(TARGET)

# Run the client with default arguments
run: $(TARGET)
	./$(TARGET) 

# Phony targets
.PHONY: all clean run