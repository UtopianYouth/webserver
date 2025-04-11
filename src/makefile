# 开发框架头文件路径
PUBINCL = -I/home/utopianyouth/webserver/include

# 开发框架 cpp 文件名，直接和程序的源代码文件一起编译，没有采用链接库，是为了方便调试
PUBCPP = /home/utopianyouth/webserver/src/http_connection.cpp


# 编译选项
CFLAGS = -g
# CLFAGS = -O2

all: main

main: main.cpp http_connection.cpp
	g++ $(CFLAGS) main.cpp -o webserver $(PUBINCL) $(PUBCPP) -lpthread
	cp -f webserver ../bin/webserver

clean:
	rm -rf webserver