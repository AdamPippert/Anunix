#ifndef ANX_RESEARCH_TEST_H
#define ANX_RESEARCH_TEST_H

#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
/* Exercise external-call authorization without resetting live stores. */
int anx_research_day001(void);
int anx_research_day002(void);
int anx_research_day003(void);
int anx_research_day004(void);
int anx_research_day005(void);
int anx_research_day006(void);
int anx_research_day007(void);
int anx_research_day008(void);
int anx_research_day009(void);
int anx_research_day010(void);
int anx_research_day011(void);
int anx_research_day012(void);
int anx_research_day013(void);
int anx_research_day014(void);
int anx_research_day015(void);
int anx_research_day016(void);
int anx_research_day017(void);
int anx_research_day018(void);
int anx_research_day019(void);
int anx_research_day020(void);
int anx_research_day021(void);
int anx_research_day022(void);
int anx_research_day023(void);
int anx_research_day024(void);
int anx_research_day025(void);
int anx_research_day026(void);
int anx_research_day027(void);
int anx_research_day028(void);
int anx_research_day029(void);
int anx_research_day030(void);
int anx_research_day031(void);
int anx_research_day032(void);
int anx_research_day033(void);
#endif

#ifdef ANX_RESEARCH_TEST
/* Run a selected research regression in a dedicated test image. */
void cmd_research_test(int argc, char **argv);
#endif

#endif
