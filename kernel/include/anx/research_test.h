#ifndef ANX_RESEARCH_TEST_H
#define ANX_RESEARCH_TEST_H

#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
/* Exercise external-call authorization without resetting live stores. */
int anx_research_day001(void);
#endif

#ifdef ANX_RESEARCH_TEST
/* Run a selected research regression in a dedicated test image. */
void cmd_research_test(int argc, char **argv);
#endif

#endif
