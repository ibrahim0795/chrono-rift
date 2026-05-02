CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

# SFML GUI
LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt

# .bin extension to avoid collision with folder names
TARGETS = arbiter.bin hip.bin asp.bin

all: clean $(TARGETS)
	@echo Build complete.

arbiter.bin: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

hip.bin: hip/hip.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp -o $@ $(LIBS)

asp.bin: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean