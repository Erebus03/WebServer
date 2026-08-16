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
# Re-derived again 2026-08-16: ALL 17 suites below define a main(), all 17 build,
# and `make test` runs all 17. A paragraph here used to claim seven of them were
# inert; it had drifted for the third time. This list is DERIVED, not maintained --
# re-run the command above rather than editing from memory.
TEST_SRCS = \
	tests/test_cgi_response.cpp \
	tests/test_config.cpp \
	tests/test_DeleteHandler.cpp \
	tests/test_DirectoryLister.cpp \
	tests/test_Dispatcher.cpp \
	tests/test_FileUtils.cpp \
	tests/test_GetHandler.cpp \
	tests/test_http_parser.cpp \
	tests/test_HttpStatus.cpp \
	tests/test_integration.cpp \
	tests/test_mime_types.cpp \
	tests/test_multipart.cpp \
	tests/test_PostHandler.cpp \
	tests/test_response_builder.cpp \
	tests/test_router.cpp \
	tests/test_stream_hooks.cpp \
	tests/test_stream_hooks_adversarial.cpp \

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

# One runner covering the ground the school tester, cgi_tester and the third-party
# testers each cover separately, PLUS the config parser, which none of them touch.
# Every line states what it checks; failures print want-vs-got. Exit status is the
# failure count. Python 3 stdlib only -- raw sockets, so it can send the malformed
# requests curl cannot express.
fulltest: $(NAME)
	@python3 tests/fulltest.py $(ARGS)

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

.PHONY: all clean fclean re test fulltest stress valgrind
