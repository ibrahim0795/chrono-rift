CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

# SFML GUI
LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt

# Shared utilities compiled into every binary
SHARED_SRC = shared/shared_utils.cpp

# .bin extension to avoid collision with folder names
TARGETS = arbiter.bin hip.bin asp.bin

all: clean $(TARGETS)
	@echo Build complete.

arbiter.bin: arbiter/arbiter.cpp $(SHARED_SRC)
	$(CXX) $(CXXFLAGS) arbiter/*.cpp $(SHARED_SRC) -o $@ $(LIBS)

hip.bin: hip/hip.cpp $(SHARED_SRC)
	$(CXX) $(CXXFLAGS) hip/*.cpp $(SHARED_SRC) -o $@ $(LIBS)

asp.bin: asp/asp.cpp $(SHARED_SRC)
	$(CXX) $(CXXFLAGS) asp/*.cpp $(SHARED_SRC) -o $@ $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean