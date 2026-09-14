/*
 *
 * compile options.
 *
 */
// Edit this file to customize when compiling with Arduino IDE
// Sets of these flags are also defined in platformio.ini

#ifndef OC_OPTIONS_H_
#define OC_OPTIONS_H_


/* ------------ uncomment for boring app names ------------------------------------------------------  */
//#define BORING_APP_NAMES
/* ------------ print debug messages to USB serial --------------------------------------------------  */
//#define PRINT_DEBUG
/* ------------ invert screen pixels ----------------------------------------------------------------  */
//#define INVERT_DISPLAY
/* ------------ use DAC8564 -------------------------------------------------------------------------  */
//#define DAC8564
/* ------------ 0 / 10V range -----------------------------------------------------------------------  */
//#define IO_10V
/* ------------ Debug for autotune ------------------------------------------------------------------  */
//#define AUTOTUNE_DEBUG
/* ------------ Debug for app load/save -------------------------------------------------------------  */
//#define APPS_DEBUG



// idk what this means so I'm keeping it -NJM

// backward compatibility

// Here are some custom flags:
/* --- special Phazerville mode w/ easter eggs --- */
// #define PEWPEWPEW
/* --- alternate Grids patterns for DrumMap applet --- */
// #define DRUMMAP_GRIDS2
// 16 presets in Hemisphere
// #define MOAR_PRESETS


/* Flags for the full-width apps, these enable/disable them in OC_apps.ino but also zero out the app   */
/* files to prevent them from taking up space.                                                         */

// #define ENABLE_APP_CALIBR8OR
// #define ENABLE_APP_ENIGMA
// #define ENABLE_APP_MIDI
// #define ENABLE_APP_PIQUED
// #define ENABLE_APP_POLYLFO
// #define ENABLE_APP_H1200
// #define ENABLE_APP_BYTEBEATGEN
// #define ENABLE_APP_NEURAL_NETWORK
// #define ENABLE_APP_DARKEST_TIMELINE
// #define ENABLE_APP_LORENZ
// #define ENABLE_APP_ASR
// #define ENABLE_APP_QUANTERMAIN
// #define ENABLE_APP_METAQ
// #define ENABLE_APP_CHORDS
// #define ENABLE_APP_PASSENCORE
// #define ENABLE_APP_SEQUINS
// #define ENABLE_APP_AUTOMATONNETZ
// #define ENABLE_APP_BBGEN
// #define ENABLE_APP_REFERENCES

// Disable Hemisphere and all the applets, freeing up space.
// If you really want to squeeze everything else in, use this.
// To exclude individual applets, edit the Registry<> list in applets/_config.h
// #define NO_HEMISPHERE

#endif

