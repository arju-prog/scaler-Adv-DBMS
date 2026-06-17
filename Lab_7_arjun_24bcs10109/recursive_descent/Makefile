CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
TARGET   := rd
SOURCES  := main.cpp

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o

.PHONY: all run clean
