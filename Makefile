# Configuration

TARGET      = webserv
TEST        = test

SRC_DIR     = src
TST_DIR     = tst
INC_DIR     = inc
OBJ_DIR     = obj
BIN_DIR     = bin

NAME        = $(TARGET)
TEST_NAME   = $(BIN_DIR)/$(TEST)

CXX         = c++
CXXFLAGS    = -Wall -Wextra -Werror -std=c++98 -MMD -MP $(FLAGS)
INCLUDES    = -I$(INC_DIR)

SRCS        = $(wildcard $(SRC_DIR)/*.cpp)
TEST_SRCS   = $(wildcard $(TST_DIR)/*.cpp)
OBJS        = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))
TEST_OBJS   = $(patsubst $(TST_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(TEST_SRCS))
MAIN_OBJ    = $(OBJ_DIR)/main.o
DEPS        = $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(MAIN_OBJ:.o=.d)

ifeq ($(shell uname -s),Darwin)
CXXFLAGS   += -DUSE_KQUEUE
endif

# Build

all: $(NAME)

kqueue: CXXFLAGS += -DUSE_KQUEUE
kqueue: fclean all

test: $(TEST_NAME)

$(NAME): $(OBJS) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(OBJS) $(MAIN_OBJ) -o $(NAME)

$(TEST_NAME): $(OBJS) $(TEST_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(OBJS) $(TEST_OBJS) -o $(TEST_NAME)

$(OBJ_DIR)/%.o: $(TST_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(MAIN_OBJ): app/main.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Directories

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# ==============================================================================
# Special Rules
# ==============================================================================
mac:
	@echo "Starting Ubuntu development container..."
	@docker run --rm -it \
		-p 8080:8080 \
		-v "$(shell pwd)":/workspace \
		-w /workspace \
		ubuntu:24.04 \
		bash -lc '\
			export DEBIAN_FRONTEND=noninteractive; \
			apt-get update; \
			apt-get install -y --no-install-recommends \
				build-essential clang make git \
				python3 python3-pip python3-venv ca-certificates; \
			python3 -m venv /tmp/venv; \
			. /tmp/venv/bin/activate; \
			python -m pip install --quiet requests; \
			exec bash'

# Same container but x86-64 (emulated on Apple Silicon), to run the official
# 42 tester and cgi_tester, which are Linux x86-64 binaries. Inside:
#   make re && bash tst/tests_42/setup.sh
#   ./webserv tst/tests_42/tester.conf &  ./tst/tests_42/tester http://127.0.0.1:8080
mac-x86:
	@echo "Starting Ubuntu x86-64 container (emulated) for the 42 tester..."
	@docker run --rm -it \
		--platform linux/amd64 \
		-p 8080:8080 \
		-v "$(shell pwd)":/workspace \
		-w /workspace \
		ubuntu:24.04 \
		bash -lc '\
			export DEBIAN_FRONTEND=noninteractive; \
			apt-get update; \
			apt-get install -y --no-install-recommends \
				build-essential clang make git curl \
				python3 python3-pip python3-venv ca-certificates; \
			python3 -m venv /tmp/venv; \
			. /tmp/venv/bin/activate; \
			python -m pip install --quiet requests; \
			exec bash'

# ==============================================================================
# Cleanup Rules
# ==============================================================================
clean:
	rm -rf $(OBJ_DIR)

fclean: clean
	rm -rf $(BIN_DIR) $(NAME)

re: fclean all

# Recompile objects when a header they include changes
-include $(DEPS)

# Phony targets
.PHONY: all clean fclean re test mac mac-x86 kqueue
