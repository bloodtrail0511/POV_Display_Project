# g++ -o pov_pc_sim_v2 pov_pc_sim_v2.cpp $(pkg-config --cflags --libs opencv4)
# ./pov_pc_sim_v2

g++ -o app_pong_sim app_pong_sim.cpp $(pkg-config --cflags --libs opencv4)
./app_pong_sim

# g++ -o claude_pong claude_pong.cpp $(pkg-config --cflags --libs opencv4)
# ./claude_pong