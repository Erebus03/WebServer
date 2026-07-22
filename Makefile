NAME = webserv

CXX = c++
CXXFLAGS = -Wall -Wextra -Werror -g -std=c++98

SRCS = \
	src/main.cpp \
	src/Router.cpp \
	src/Dispatcher.cpp \
	src/Config.cpp \
	src/HttpParser.cpp \
	src/Client.cpp \
	src/Server.cpp \
	src/GetHandler.cpp \
	src/PostHandler.cpp \
	src/DeleteHandler.cpp \
	src/CgiHandler.cpp \
	src/DirectoryLister.cpp \
	src/FileUtils.cpp \
	tests/test_router.cpp \
	tests/test_FileUtils.cpp \
	tests/test_GetHandler.cpp \

OBJS = $(SRCS:.cpp=.o)

INCLUDES = \
	includes/types.hpp \
	includes/Router.hpp \
	includes/Dispatcher.hpp \
	includes/Client.hpp \
	includes/Config.hpp \
	includes/GetHandler.hpp \
	includes/HttpParser.hpp \
	includes/DeleteHandler.hpp \
	includes/Server.hpp \
	includes/PostHandler.hpp \
	includes/CgiHandler.hpp \
	includes/DirectoryLister.hpp \
	includes/FileUtils.hpp \

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(NAME)

%.o: %.cpp $(INCLUDES)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
