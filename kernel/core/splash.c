/*
 * splash.c — Boot splash screen for serial console.
 *
 * Displays the Anunix logo using ANSI escape codes.
 * Works on any terminal that supports 256-color ANSI
 * (QEMU serial, UTM console, minicom, etc.).
 */

#include <anx/arch.h>
#include <anx/kprintf.h>

/*
 * ANSI color codes for the teal-to-navy gradient.
 * Uses 256-color mode: \033[38;5;Nm
 *   30 = dark teal    37 = teal       73 = medium teal
 *   31 = darker teal  23 = navy       24 = dark navy
 */

static void puts_color(const char *s, int color)
{
	kprintf("\033[38;5;%dm%s\033[0m", color, s);
}

void anx_splash(void)
{
	/* Clear screen */
	kprintf("\033[2J\033[H");

	kprintf("\n");

	/*
	 * Anunix "A" logo — stylized triangular mark with center dot.
	 * Uses block drawing characters and ANSI 256-color for the
	 * teal-to-navy gradient from the brand logo.
	 */

	/* Row 1: peak of the A */
	kprintf("                       ");
	puts_color("___", 37);
	kprintf("\n");

	/* Row 2 */
	kprintf("                      ");
	puts_color("/", 37);
	kprintf("   ");
	puts_color("\\", 37);
	kprintf("\n");

	/* Row 3 */
	kprintf("                     ");
	puts_color("/", 37);
	kprintf("  ");
	puts_color("o", 6);
	kprintf("  ");
	puts_color("\\", 73);
	kprintf("\n");

	/* Row 4 */
	kprintf("                    ");
	puts_color("/", 30);
	kprintf("       ");
	puts_color("\\", 73);
	kprintf("\n");

	/* Row 5: crossbar */
	kprintf("                   ");
	puts_color("/", 30);
	kprintf("  ");
	puts_color("_____", 37);
	kprintf("  ");
	puts_color("\\", 73);
	kprintf("\n");

	/* Row 6 */
	kprintf("                  ");
	puts_color("/", 24);
	kprintf("  ");
	puts_color("/", 30);
	kprintf("     ");
	puts_color("\\", 73);
	kprintf("  ");
	puts_color("\\", 73);
	kprintf("\n");

	/* Row 7 */
	kprintf("                 ");
	puts_color("/", 24);
	kprintf("  ");
	puts_color("/", 24);
	kprintf("       ");
	puts_color("\\", 37);
	kprintf("  ");
	puts_color("\\", 37);
	kprintf("\n");

	/* Row 8: base */
	kprintf("                ");
	puts_color("/", 24);
	puts_color("__", 24);
	puts_color("/", 24);
	kprintf("         ");
	puts_color("\\", 37);
	puts_color("__", 37);
	puts_color("\\", 37);
	kprintf("\n");

	kprintf("\n");

	/* Wordmark */
	kprintf("                ");
	puts_color("A N U N I X", 37);
	kprintf("\n");

	/* Tagline */
	kprintf("          ");
	puts_color("The AI-Native Operating System", 243);
	kprintf("\n");

	kprintf("\n");
}
