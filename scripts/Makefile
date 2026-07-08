CC = g++
CFLAGS = -Wall -O3 -std=c++17
LIBS = -lpulse -lpulse-simple

TARGET = rtp_pc_sender
SRC = rtp_pc_sender.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
