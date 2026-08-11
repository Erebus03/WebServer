NAME = webserv

CXX = c++
CXXFLAGS = -Wall -Wextra -Werror -g -std=c++98

SRCS = \
	src/main.cpp \
	src/Router.cpp \
	src/Dispatcher.cpp \
	src/Config.cpp \
	src/HttpParser.cpp \
	src/HttpVersion.cpp \
	src/HttpStatus.cpp \
	src/ResponseBuilder.cpp \
	src/MimeTypes.cpp \
	src/UrlCodec.cpp \
	src/MultipartParser.cpp \
	src/CgiResponse.cpp \
	src/Client.cpp \
	src/Server.cpp \
	src/GetHandler.cpp \
	src/PostHandler.cpp \
	src/DeleteHandler.cpp \
	src/DirectoryLister.cpp \
	src/FileUtils.cpp \

OBJS = $(SRCS:.cpp=.o)

INCLUDES = \
	includes/types.hpp \
	includes/Router.hpp \
	includes/Dispatcher.hpp \
	includes/Client.hpp \
	includes/Config.hpp \
	includes/GetHandler.hpp \
	includes/HttpParser.hpp \
	includes/HttpVersion.hpp \
	includes/HttpStatus.hpp \
	includes/ResponseBuilder.hpp \
	includes/MimeTypes.hpp \
	includes/UrlCodec.hpp \
	includes/MultipartParser.hpp \
	includes/CgiResponse.hpp \
	includes/DeleteHandler.hpp \
	includes/Server.hpp \
	includes/PostHandler.hpp \
	includes/DirectoryLister.hpp \
	includes/FileUtils.hpp \

# Everything except the server's own entry point. Each test brings its own
# main(), so linking src/main.o too would be a duplicate-symbol error. Linking
# the whole rest of the program rather than a hand-picked subset per test is
# deliberate: a test that grows a new dependency keeps building instead of
# failing with an undefined reference nobody wants to debug.
LIB_OBJS = $(filter-out src/main.o,$(OBJS))

# Every unit test. These belong HERE and never in SRCS: each brings its own
# main(), so listing one in SRCS links it into the server binary and turns
# ./webserv into a test runner. That is exactly what used to happen — nine of
# these were in SRCS — and it is why `tests/%` is a separate target below.
# ONLY the files that define their own main(), because `tests/%` links one test
# source against LIB_OBJS and nothing else. Verified with:
#   for f in tests/test_*.cpp; do grep -qE '^\s*int\s+main\s*\(' $f && echo $f; done
#
# Re-derived 2026-08-11 with the command above, because this list had drifted
# twice: it named tests/test_PostHandler.cpp, which exists only on origin/abdo and
# not here, so `make test` died with "No rule to make target" before running
# anything; and it claimed test_http_parser has no main(), which stopped being
# true when B's suite merged. Five suites with a main() were therefore never run.
#
# The ones with NO main() (test_router, test_FileUtils, test_GetHandler,
# test_Dispatcher, test_DirectoryLister, test_HttpStatus, test_DeleteHandler) still
# cannot be built by this target: `tests/%` links one test source against LIB_OBJS
# and nothing else, so listing one fails with "undefined reference to main". They
# need a main() each or a shared runner; until then they are inert.
TEST_SRCS = \
	tests/test_cgi_response.cpp \
	tests/test_config.cpp \
	tests/test_http_parser.cpp \
	tests/test_integration.cpp \
	tests/test_mime_types.cpp \
	tests/test_multipart.cpp \
	tests/test_response_builder.cpp \

TEST_BINS = $(TEST_SRCS:.cpp=)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(NAME)

%.o: %.cpp $(INCLUDES)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# -Iincludes so a test may say "Config.hpp" as well as "../includes/Config.hpp".
tests/%: tests/%.cpp $(LIB_OBJS) $(INCLUDES)
	$(CXX) $(CXXFLAGS) -Iincludes $< $(LIB_OBJS) -o $@

# Unit tests. Stops at the first failure so a red run cannot scroll past.
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "=== $$t"; ./$$t || exit 1; \
	done
	@echo "=== all unit tests passed"

# Live-server tests: 50 concurrent clients, hostile mix. Needs the binary.
stress: $(NAME)
	@./tests/stress.sh

# Same load under valgrind, checking for leaked memory and leaked descriptors.
valgrind: $(NAME)
	@./tests/stress.sh --valgrind

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME) $(TEST_BINS) tests/dump_config

re: fclean all

.PHONY: all clean fclean re test stress valgrind
