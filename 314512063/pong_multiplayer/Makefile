CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17

all: app_pong_multi pong_client

app_pong_multi: app_pong_multi.cpp net_server.cpp net_server.hpp
	$(CXX) $(CXXFLAGS) -o app_pong_multi app_pong_multi.cpp net_server.cpp `pkg-config --cflags --libs opencv4` -pthread

pong_client: pong_client.cpp
	$(CXX) $(CXXFLAGS) -o pong_client pong_client.cpp

clean:
	rm -f app_pong_multi pong_client