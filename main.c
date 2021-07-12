#include <stdio.h>
#include <stdlib.h>
#include "src/syntax/token.h"
#include "src/syntax/scanner.h"
#include "src/syntax/parser.h"

static char *open(char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "Could not open file \"%s\".\n", path);
    exit(74);
  }

  fseek(file, 0L, SEEK_END);
  size_t fileSize = ftell(file);
  rewind(file);

  char *buffer = (char *) malloc(fileSize + 1);
  if (buffer == NULL) {
    fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
    exit(74);
  }

  size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);

  if (bytesRead < fileSize) {
    fprintf(stderr, "Could not read file \"%s\".\n", path);
    exit(74);
  }

  buffer[bytesRead] = '\0';

  fclose(file);
  return buffer;
}

int main() {
  // 读取文件
  char *source = open("/Users/weiwenhao/Code/nature/example/test.n");

  // scanner
  list *token_list = scanner(source);
  while (!list_empty(token_list)) {
    token *t = list_pop(token_list);
    printf("type: %d, literal:%s\n", t->type, t->literal);
  }

  // parser
  ast_block_stmt stmt_list = parser(token_list);
  for (int i = 0; i < stmt_list.count; ++i) {
    ast_stmt stmt = stmt_list.list[i];
    switch (stmt.type) {
      case AST_VAR_DECL: {

      }
    }
  }

  printf("Hello, World!\n");
  return 0;
}
