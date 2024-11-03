#!/bin/bash

# Ensure the script exits on any error
set -e

# Define the source file
SOURCE_FILE="test.c"

# i) Source to Binary
echo "Compiling source to binary..."
clang "$SOURCE_FILE" -o test

# ii) Source to Object File
echo "Compiling source to object file..."
clang -c "$SOURCE_FILE" -o test.o

# iii) Source to Machine Assembly
echo "Compiling source to machine assembly..."
clang -S "$SOURCE_FILE" -o test.s

# iv) Source to LLVM Bitcode
echo "Compiling source to LLVM bitcode..."
clang -c -emit-llvm "$SOURCE_FILE" -o test.bc

# iv) Source to LLVM IR
echo "Compiling source to LLVM IR..."
clang -S -emit-llvm "$SOURCE_FILE" -o test.ll

# v) LLVM IR to LLVM Bitcode
echo "Compiling LLVM IR to LLVM bitcode..."
clang -c -emit-llvm test.ll -o test.bc

# vi) LLVM Bitcode to LLVM IR
echo "Converting LLVM bitcode to LLVM IR..."
llvm-dis test.bc -o test.ll

# vii) LLVM IR to Machine Assembly
echo "Compiling LLVM IR to machine assembly..."
clang -S test.ll -o test.s

echo "Compilation process completed successfully!"
