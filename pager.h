#ifndef FASTLESS_PAGER_H
#define FASTLESS_PAGER_H
/* Each takes the file arguments ("-" is standard input) and returns the
   exit status. */
int pager_run(char **names, int count);
int pager_cat(char **names, int count);
int pager_find(char **names, int count);
/* Reads standard input once; every "-" argument shares it. */
int pager_load_stdin(void);
#define FASTLESS_VERSION "fastless 1.0"
#endif
