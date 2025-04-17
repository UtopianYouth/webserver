# 开发框架头文件路径
PUBINCL = -I/home/utopianyouth/webserver/include

# 开发框架 cpp 文件名，直接和程序的源代码文件一起编译，没有采用链接库，是为了方便调试
PUBCPP1 = /home/utopianyouth/webserver/src/http_connection.cpp
PUBCPP2 = /home/utopianyouth/webserver/src/lst_timer.cpp



# 编译选项
CFLAGS = -g
# CLFAGS = -O2

all: main nonactive_connection

main: main.cpp http_connection.cpp lst_timer.cpp
	g++ $(CFLAGS) main.cpp -o webserver $(PUBINCL) $(PUBCPP1) $(PUBCPP2) -lpthread
	cp -f webserver ../bin/webserver

nonactive_connection: nonactive_connection.cpp lst_timer.cpp
	g++ $(CFLAGS) nonactive_connection.cpp -o nonactive_connection $(PUBCPP2) -lpthread
	
clean:
	/bin/rm -rf webserver