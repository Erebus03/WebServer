NAME = webserv

CXX = c++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98

SRCS = \
	src/main.cpp \
	src/Router.cpp \
	src/Config.cpp \
	src/HttpParser.cpp \
	tests/test_router.cpp \

OBJS = $(SRCS:.cpp=.o)

INCLUDES = \
	includes/types.hpp \
	includes/Router.hpp \

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
