/*
 * HD2 Macropad Link - portable core (frame parser + HID report -> scancode events).
 * Shared by the Windows app and the Linux unit test.
 */
#pragma once
#include <stdint.h>
#include <string.h>

#define LINK_T_KEY   0x01
#define LINK_T_PONG  0x02
#define LINK_T_PING  0x81

/* ---------------------------------------------------------------- frames */

typedef struct {
    int state;
    uint8_t type, len, idx, sum;
    uint8_t payload[16];
} link_parser_t;

/* Feed one byte. Returns 1 when a valid frame completed (type/len/payload filled). */
static int link_parse_byte(link_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case 0: p->state = (b == 0xA5) ? 1 : 0; return 0;
    case 1: p->state = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); return 0;
    case 2: p->type = b; p->sum = b; p->state = 3; return 0;
    case 3:
        p->len = b; p->sum += b; p->idx = 0;
        p->state = (b > 16) ? 0 : (b == 0 ? 5 : 4);
        return 0;
    case 4:
        p->payload[p->idx++] = b; p->sum += b;
        if (p->idx >= p->len) p->state = 5;
        return 0;
    case 5:
        p->state = 0;
        return ((uint8_t)~p->sum == b) ? 1 : 0;
    }
    p->state = 0;
    return 0;
}

static int link_build_frame(uint8_t *out, uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t sum = type + len;
    out[0] = 0xA5; out[1] = 0x5A; out[2] = type; out[3] = len;
    for (uint8_t i = 0; i < len; i++) { out[4 + i] = payload[i]; sum += payload[i]; }
    out[4 + len] = (uint8_t)~sum;
    return 5 + len;
}

/* ---------------------------------------------------------------- HID usage -> PC/AT set 1 scancode */
/* value: low byte = scancode, 0x100 bit = extended (E0 prefix). 0 = unmapped */

static uint16_t hid_to_scancode(uint8_t u)
{
    static const uint16_t t[0x74] = {
        /* 0x00 */ 0, 0, 0, 0,
        /* a-z 0x04-0x1D */
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
        /* 1-9,0 0x1E-0x27 */
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        /* 0x28 Enter, Esc, Bksp, Tab, Space, -, =, [, ], \, NonUS#, ;, ', `, ',', ., / */
        0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35,
        /* 0x39 Caps */ 0x3A,
        /* 0x3A-0x45 F1-F12 */
        0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
        /* 0x46 PrtSc, ScrLk, Pause */ 0x137, 0x46, 0,
        /* 0x49 Ins, Home, PgUp, Del, End, PgDn */ 0x152, 0x147, 0x149, 0x153, 0x14F, 0x151,
        /* 0x4F Right, Left, Down, Up */ 0x14D, 0x14B, 0x150, 0x148,
        /* 0x53 NumLock, KP/, KP*, KP-, KP+, KPEnter */ 0x45, 0x135, 0x37, 0x4A, 0x4E, 0x11C,
        /* 0x59-0x61 KP1-KP9 */ 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49,
        /* 0x62 KP0, KP. */ 0x52, 0x53,
        /* 0x64 NonUS\, App, Power, KP= */ 0x56, 0x15D, 0, 0,
        /* 0x68-0x73 F13-F24 */ 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x76,
    };
    return u < 0x74 ? t[u] : 0;
}

/* modifier bit (0..7: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui) -> scancode */
static uint16_t mod_to_scancode(int bit)
{
    static const uint16_t m[8] = {0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C};
    return m[bit & 7];
}

/* ---------------------------------------------------------------- report diff */

typedef void (*link_key_cb)(uint16_t scancode, int down, void *ctx);

typedef struct {
    uint8_t mask;
    uint8_t keys[6];
} link_state_t;

static int link_has(const uint8_t *k, uint8_t u)
{
    for (int i = 0; i < 6; i++) if (k[i] == u) return 1;
    return 0;
}

/* Apply a new report: releases first, then presses (modifiers pressed before keys). */
static void link_apply_report(link_state_t *s, const uint8_t *rep, link_key_cb cb, void *ctx)
{
    uint8_t mask = rep[0];
    const uint8_t *keys = rep + 1;

    for (int i = 0; i < 6; i++) {
        uint8_t u = s->keys[i];
        if (u && !link_has(keys, u)) {
            uint16_t sc = hid_to_scancode(u);
            if (sc) cb(sc, 0, ctx);
        }
    }
    for (int b = 0; b < 8; b++) {
        if ((s->mask & (1 << b)) && !(mask & (1 << b))) cb(mod_to_scancode(b), 0, ctx);
    }
    for (int b = 0; b < 8; b++) {
        if (!(s->mask & (1 << b)) && (mask & (1 << b))) cb(mod_to_scancode(b), 1, ctx);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t u = keys[i];
        if (u && !link_has(s->keys, u)) {
            uint16_t sc = hid_to_scancode(u);
            if (sc) cb(sc, 1, ctx);
        }
    }
    s->mask = mask;
    memcpy(s->keys, keys, 6);
}

static void link_release_all(link_state_t *s, link_key_cb cb, void *ctx)
{
    const uint8_t empty[7] = {0};
    link_apply_report(s, empty, cb, ctx);
}
