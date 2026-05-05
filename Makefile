NAME = webserv
CXX = c++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98
INC_DIR = includes
SRC_DIR = src
OBJ_DIR = obj

# Source files
SOURCES = main_new.cpp \
          src/Server.cpp \
          src/Client.cpp \
          src/Http.cpp \
          src/Config.cpp \
          src/Router.cpp

# Object files
OBJECTS = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES))

all: $(NAME)

$(NAME): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $(NAME) $(OBJECTS)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
