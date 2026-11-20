#ifndef TEXTINPUT_H
#define TEXTINPUT_H

/* Lets the user type one line of text, starting from what is in buf.
 * Uses the console's own on-screen keyboard when there is one, otherwise a
 * simple built-in one. Returns 1 if the text was confirmed (buf updated),
 * 0 if it was cancelled. redraw() must draw the screen behind the keyboard
 * and flip; it is called once per frame while the system keyboard is open. */
int text_input(const char *title, char *buf, int buf_size, int password, void (*redraw)(void));

#endif
