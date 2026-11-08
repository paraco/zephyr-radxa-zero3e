cmake_minimum_required(VERSION 3.20.0)
# L9d — DRAM 꼭대기(0x78000000~0x80000000)에서 U-Boot 가 쓰는 곳을 찾는다 (블롭 창을 576 MiB 로 늘리기 전).
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llm_l9d)
target_sources(app PRIVATE src/main.c)
