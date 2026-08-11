#ifndef WEB_H
#define WEB_H

/*
 * HTTP + WebSocket console.
 *
 * Started once the device has an IP address. Serves a single-page terminal at
 * "/" and accepts command lines over a WebSocket at "/ws". Commands from the
 * browser are pushed onto the same queue the serial reader feeds, and all
 * command output is mirrored to every connected browser -- which is what makes
 * the README's "output of any command is sent to both interfaces" true.
 */
void bp_web_start(void);
void bp_web_stop(void);

#endif
