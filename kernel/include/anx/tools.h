/*
 * anx/tools.h — ansh tool entry points.
 *
 * Each tool is implemented in kernel/core/tools/<name>.c and
 * exports a cmd_<name>(int argc, char **argv) function.
 * The ansh dispatch table references these.
 */

#ifndef ANX_TOOLS_H
#define ANX_TOOLS_H

/* Phase 1: Core tools */
void cmd_ls(int argc, char **argv);
void cmd_cat(int argc, char **argv);
void cmd_write_obj(int argc, char **argv);
void cmd_cp(int argc, char **argv);
void cmd_mv(int argc, char **argv);
void cmd_rm_obj(int argc, char **argv);
void cmd_cells(int argc, char **argv);
void cmd_sysinfo(int argc, char **argv);

/* Phase 2: Productive tools */
void cmd_search(int argc, char **argv);
void cmd_inspect(int argc, char **argv);
void cmd_fetch(int argc, char **argv);
void cmd_netinfo(int argc, char **argv);

/* Interface Plane tools */
void cmd_surfctl(int argc, char **argv);
void cmd_evctl(int argc, char **argv);
void cmd_compctl(int argc, char **argv);
void cmd_envctl(int argc, char **argv);

/* Hardware discovery agent */
void cmd_hwd(int argc, char **argv);

/* Metadata tool (Phase 2) */
void cmd_meta(int argc, char **argv);

/* Tensor tool (RFC-0013) */
void cmd_tensor(int argc, char **argv);

/* Model tool (RFC-0013) */
void cmd_model(int argc, char **argv);

/* WiFi management */
void cmd_wifi(int argc, char **argv);

/* Display diagnostics */
void cmd_fb_info(int argc, char **argv);
void cmd_gop_list(int argc, char **argv);
void cmd_fb_test(int argc, char **argv);

/* Browser Renderer Cell */
void cmd_browser_init(int argc, char **argv);
void cmd_browser(int argc, char **argv);
void cmd_browser_stop(int argc, char **argv);

#endif /* ANX_TOOLS_H */
