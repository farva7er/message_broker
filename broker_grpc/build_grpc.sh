#!/bin/bash

protoc --grpc_out=. --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` ./broker_grpc.proto
protoc --cpp_out=. ./broker_grpc.proto
