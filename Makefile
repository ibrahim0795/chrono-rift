CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

# SFML GUI
LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt

# Shared utilities compiled into every binary
SHARED_SRC = shared/shared_utils.cpp

# Google Test
GTEST_LIBS = -lgtest -lgtest_main -lpthread -lrt

# .bin extension to avoid collision with folder names
TARGETS = arbiter.bin hip.bin asp.bin

all: clean $(TARGETS)
	@echo Build complete.

arbiter.bin: arbiter/arbiter.cpp game_logic.cpp $(SHARED_SRC)
	$(CXX) $(CXXFLAGS) arbiter/*.cpp game_logic.cpp $(SHARED_SRC) -o $@ $(LIBS)

hip.bin: hip/hip.cpp game_logic.cpp $(SHARED_SRC) arbiter/weapon_names.cpp
	$(CXX) $(CXXFLAGS) hip/*.cpp game_logic.cpp $(SHARED_SRC) arbiter/weapon_names.cpp -o $@ $(LIBS)

asp.bin: asp/asp.cpp game_logic.cpp $(SHARED_SRC) arbiter/weapon_names.cpp
	$(CXX) $(CXXFLAGS) asp/*.cpp game_logic.cpp $(SHARED_SRC) arbiter/weapon_names.cpp -o $@ $(LIBS)

# Unit tests (no SFML dependency)
test: tests/test_game_logic.bin
	./tests/test_game_logic.bin

tests/test_game_logic.bin: tests/test_game_logic.cpp game_logic.cpp arbiter/weapon_names.cpp
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -I. tests/test_game_logic.cpp game_logic.cpp arbiter/weapon_names.cpp -o $@ $(GTEST_LIBS)

clean:
	rm -f $(TARGETS) tests/*.bin

.PHONY: all clean test
