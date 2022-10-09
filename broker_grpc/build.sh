#!/bin/bash

./build_grpc.sh
g++ -Wall -g main.cpp broker_grpc.pb.cc broker_grpc.grpc.pb.cc \
   `pkg-config --libs protobuf grpc++` -lpthread -ldl -o broker
