/**
 * insertion.c
 * Insertion effects processor — port of src_ts/synthesizer/audio_engine/effects/insertion/
 *
 * Effects: Thru, StereoEQ, Phaser, AutoPan, Tremolo, AutoWah, PhAutoWah
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if __has_include(<spessasynth_core/insertion.h>)
#include <spessasynth_core/insertion.h>
#else
#include "spessasynth/synthesizer/dsp/insertion.h"
#endif

/* ── Conversion table (InsertionValueConverter) ───────────────────────────── */
/* Columns: PreDly,Dly1,Dly2,Dly3,Dly4, Rate1,Rate2, HF,Cut,EQ,LPF,Manual,Azim,Accl */
static const float IVC[128][14] = {
	{ 0.0f, 200, 200, 0.0f, 0, 0.05f, 0.05f, 315, 250, 200, 250, 100, -180, 0 },
	{ 0.1f, 205, 205, 0.1f, 5, 0.10f, 0.10f, 315, 250, 200, 250, 110, -180, 1 },
	{ 0.2f, 210, 210, 0.2f, 10, 0.15f, 0.15f, 315, 250, 200, 250, 120, -180, 2 },
	{ 0.3f, 215, 215, 0.3f, 15, 0.20f, 0.20f, 315, 250, 200, 250, 130, -180, 3 },
	{ 0.4f, 220, 220, 0.4f, 20, 0.25f, 0.25f, 315, 250, 200, 250, 140, -180, 4 },
	{ 0.5f, 225, 225, 0.5f, 25, 0.30f, 0.30f, 315, 250, 200, 250, 150, -180, 5 },
	{ 0.6f, 230, 230, 0.6f, 30, 0.35f, 0.35f, 315, 250, 200, 250, 160, -168, 5 },
	{ 0.7f, 235, 235, 0.7f, 35, 0.40f, 0.40f, 315, 250, 200, 250, 170, -168, 5 },
	{ 0.8f, 240, 240, 0.8f, 40, 0.45f, 0.45f, 400, 315, 250, 315, 180, -168, 5 },
	{ 0.9f, 245, 245, 0.9f, 45, 0.50f, 0.50f, 400, 315, 250, 315, 190, -168, 5 },
	{ 1.0f, 250, 250, 1.0f, 50, 0.55f, 0.55f, 400, 315, 250, 315, 200, -156, 5 },
	{ 1.1f, 255, 255, 1.1f, 55, 0.60f, 0.60f, 400, 315, 250, 315, 210, -156, 5 },
	{ 1.2f, 260, 260, 1.2f, 60, 0.65f, 0.65f, 400, 315, 250, 315, 220, -156, 5 },
	{ 1.3f, 265, 265, 1.3f, 65, 0.70f, 0.70f, 400, 315, 250, 315, 230, -156, 5 },
	{ 1.4f, 270, 270, 1.4f, 70, 0.75f, 0.75f, 400, 315, 250, 315, 240, -144, 5 },
	{ 1.5f, 275, 275, 1.5f, 75, 0.80f, 0.80f, 400, 315, 250, 315, 250, -144, 5 },
	{ 1.6f, 280, 280, 1.6f, 80, 0.85f, 0.85f, 500, 400, 315, 400, 260, -144, 5 },
	{ 1.7f, 285, 285, 1.7f, 85, 0.90f, 0.90f, 500, 400, 315, 400, 270, -144, 5 },
	{ 1.8f, 290, 290, 1.8f, 90, 0.95f, 0.95f, 500, 400, 315, 400, 280, -132, 5 },
	{ 1.9f, 295, 295, 1.9f, 95, 1.00f, 1.00f, 500, 400, 315, 400, 290, -132, 5 },
	{ 2.0f, 300, 300, 2.0f, 100, 1.05f, 1.05f, 500, 400, 315, 400, 300, -132, 5 },
	{ 2.1f, 305, 305, 2.1f, 105, 1.10f, 1.10f, 500, 400, 315, 400, 320, -132, 5 },
	{ 2.2f, 310, 310, 2.2f, 110, 1.15f, 1.15f, 500, 400, 315, 400, 340, -120, 5 },
	{ 2.3f, 315, 315, 2.3f, 115, 1.20f, 1.20f, 500, 400, 315, 400, 360, -120, 5 },
	{ 2.4f, 320, 320, 2.4f, 120, 1.25f, 1.25f, 630, 500, 400, 500, 380, -120, 5 },
	{ 2.5f, 325, 325, 2.5f, 125, 1.30f, 1.30f, 630, 500, 400, 500, 400, -120, 5 },
	{ 2.6f, 330, 330, 2.6f, 130, 1.35f, 1.35f, 630, 500, 400, 500, 420, -108, 5 },
	{ 2.7f, 335, 335, 2.7f, 135, 1.40f, 1.40f, 630, 500, 400, 500, 440, -108, 5 },
	{ 2.8f, 340, 340, 2.8f, 140, 1.45f, 1.45f, 630, 500, 400, 500, 460, -108, 5 },
	{ 2.9f, 345, 345, 2.9f, 145, 1.50f, 1.50f, 630, 500, 400, 500, 480, -108, 5 },
	{ 3.0f, 350, 350, 3.0f, 150, 1.55f, 1.55f, 630, 500, 400, 500, 500, -96, 6 },
	{ 3.1f, 355, 355, 3.1f, 155, 1.60f, 1.60f, 630, 500, 400, 500, 520, -96, 6 },
	{ 3.2f, 360, 360, 3.2f, 160, 1.65f, 1.65f, 800, 630, 500, 630, 540, -96, 6 },
	{ 3.3f, 365, 365, 3.3f, 165, 1.70f, 1.70f, 800, 630, 500, 630, 560, -96, 6 },
	{ 3.4f, 370, 370, 3.4f, 170, 1.75f, 1.75f, 800, 630, 500, 630, 580, -84, 6 },
	{ 3.5f, 375, 375, 3.5f, 175, 1.80f, 1.80f, 800, 630, 500, 630, 600, -84, 6 },
	{ 3.6f, 380, 380, 3.6f, 180, 1.85f, 1.85f, 800, 630, 500, 630, 620, -84, 6 },
	{ 3.7f, 385, 385, 3.7f, 185, 1.90f, 1.90f, 800, 630, 500, 630, 640, -84, 6 },
	{ 3.8f, 390, 390, 3.8f, 190, 1.95f, 1.95f, 800, 630, 500, 630, 660, -72, 6 },
	{ 3.9f, 395, 395, 3.9f, 195, 2.00f, 2.00f, 800, 630, 500, 630, 680, -72, 6 },
	{ 4.0f, 400, 400, 4.0f, 200, 2.05f, 2.05f, 1000, 800, 630, 800, 700, -72, 6 },
	{ 4.1f, 405, 405, 4.1f, 205, 2.10f, 2.10f, 1000, 800, 630, 800, 720, -72, 6 },
	{ 4.2f, 410, 410, 4.2f, 210, 2.15f, 2.15f, 1000, 800, 630, 800, 740, -60, 6 },
	{ 4.3f, 415, 415, 4.3f, 215, 2.20f, 2.20f, 1000, 800, 630, 800, 760, -60, 6 },
	{ 4.4f, 420, 420, 4.4f, 220, 2.25f, 2.25f, 1000, 800, 630, 800, 780, -60, 6 },
	{ 4.5f, 425, 425, 4.5f, 225, 2.30f, 2.30f, 1000, 800, 630, 800, 800, -60, 6 },
	{ 4.6f, 430, 430, 4.6f, 230, 2.35f, 2.35f, 1000, 800, 630, 800, 820, -48, 6 },
	{ 4.7f, 435, 435, 4.7f, 235, 2.40f, 2.40f, 1000, 800, 630, 800, 840, -48, 6 },
	{ 4.8f, 440, 440, 4.8f, 240, 2.45f, 2.45f, 1250, 1000, 800, 1000, 860, -48, 9 },
	{ 4.9f, 445, 445, 4.9f, 245, 2.50f, 2.50f, 1250, 1000, 800, 1000, 880, -48, 9 },
	{ 5.0f, 450, 450, 5.0f, 250, 2.55f, 2.55f, 1250, 1000, 800, 1000, 900, -36, 9 },
	{ 5.5f, 455, 455, 5.5f, 255, 2.60f, 2.60f, 1250, 1000, 800, 1000, 920, -36, 9 },
	{ 6.0f, 460, 460, 6.0f, 260, 2.65f, 2.65f, 1250, 1000, 800, 1000, 940, -36, 9 },
	{ 6.5f, 465, 465, 6.5f, 265, 2.70f, 2.70f, 1250, 1000, 800, 1000, 960, -36, 9 },
	{ 7.0f, 470, 470, 7.0f, 270, 2.75f, 2.75f, 1250, 1000, 800, 1000, 980, -24, 9 },
	{ 7.5f, 475, 475, 7.5f, 275, 2.80f, 2.80f, 1250, 1000, 800, 1000, 1000, -24, 9 },
	{ 8.0f, 480, 480, 8.0f, 280, 2.85f, 2.85f, 1600, 1250, 1000, 1250, 1100, -24, 9 },
	{ 8.5f, 485, 485, 8.5f, 285, 2.90f, 2.90f, 1600, 1250, 1000, 1250, 1200, -24, 9 },
	{ 9.0f, 490, 490, 9.0f, 290, 2.95f, 2.95f, 1600, 1250, 1000, 1250, 1300, -12, 9 },
	{ 9.5f, 495, 495, 9.5f, 295, 3.00f, 3.00f, 1600, 1250, 1000, 1250, 1400, -12, 9 },
	{ 10.0f, 500, 500, 10.0f, 300, 3.05f, 3.05f, 1600, 1250, 1000, 1250, 1500, -12, 9 },
	{ 11.0f, 505, 505, 11.0f, 305, 3.10f, 3.10f, 1600, 1250, 1000, 1250, 1600, -12, 9 },
	{ 12.0f, 510, 510, 12.0f, 310, 3.15f, 3.15f, 1600, 1250, 1000, 1250, 1700, 0, 9 },
	{ 13.0f, 515, 515, 13.0f, 315, 3.20f, 3.20f, 1600, 1250, 1000, 1250, 1800, 0, 9 },
	{ 14.0f, 520, 520, 14.0f, 320, 3.25f, 3.25f, 2000, 1600, 1250, 1600, 1900, 0, 12 },
	{ 15.0f, 525, 525, 15.0f, 325, 3.30f, 3.30f, 2000, 1600, 1250, 1600, 2000, 0, 12 },
	{ 16.0f, 530, 530, 16.0f, 330, 3.35f, 3.35f, 2000, 1600, 1250, 1600, 2100, 12, 12 },
	{ 17.0f, 535, 535, 17.0f, 335, 3.40f, 3.40f, 2000, 1600, 1250, 1600, 2200, 12, 12 },
	{ 18.0f, 540, 540, 18.0f, 340, 3.45f, 3.45f, 2000, 1600, 1250, 1600, 2300, 12, 12 },
	{ 19.0f, 545, 545, 19.0f, 345, 3.50f, 3.50f, 2000, 1600, 1250, 1600, 2400, 12, 12 },
	{ 20.0f, 550, 550, 20.0f, 350, 3.55f, 3.55f, 2000, 1600, 1250, 1600, 2500, 24, 12 },
	{ 21.0f, 560, 555, 21.0f, 355, 3.60f, 3.60f, 2000, 1600, 1250, 1600, 2600, 24, 12 },
	{ 22.0f, 570, 560, 22.0f, 360, 3.65f, 3.65f, 2500, 2000, 1600, 2000, 2700, 24, 12 },
	{ 23.0f, 580, 565, 23.0f, 365, 3.70f, 3.70f, 2500, 2000, 1600, 2000, 2800, 24, 12 },
	{ 24.0f, 590, 570, 24.0f, 370, 3.75f, 3.75f, 2500, 2000, 1600, 2000, 2900, 36, 12 },
	{ 25.0f, 600, 575, 25.0f, 375, 3.80f, 3.80f, 2500, 2000, 1600, 2000, 3000, 36, 12 },
	{ 26.0f, 610, 580, 26.0f, 380, 3.85f, 3.85f, 2500, 2000, 1600, 2000, 3100, 36, 12 },
	{ 27.0f, 620, 585, 27.0f, 385, 3.90f, 3.90f, 2500, 2000, 1600, 2000, 3200, 36, 12 },
	{ 28.0f, 630, 590, 28.0f, 390, 3.95f, 3.95f, 2500, 2000, 1600, 2000, 3300, 48, 12 },
	{ 29.0f, 640, 595, 29.0f, 395, 4.00f, 4.00f, 2500, 2000, 1600, 2000, 3400, 48, 12 },
	{ 30.0f, 650, 600, 30.0f, 400, 4.05f, 4.05f, 3150, 2500, 2000, 2500, 3500, 48, 10 },
	{ 31.0f, 660, 610, 31.0f, 405, 4.10f, 4.10f, 3150, 2500, 2000, 2500, 3600, 48, 10 },
	{ 32.0f, 670, 620, 32.0f, 410, 4.15f, 4.15f, 3150, 2500, 2000, 2500, 3700, 60, 10 },
	{ 33.0f, 680, 630, 33.0f, 415, 4.20f, 4.20f, 3150, 2500, 2000, 2500, 3800, 60, 10 },
	{ 34.0f, 690, 640, 34.0f, 420, 4.25f, 4.25f, 3150, 2500, 2000, 2500, 3900, 60, 10 },
	{ 35.0f, 700, 650, 35.0f, 425, 4.30f, 4.30f, 3150, 2500, 2000, 2500, 4000, 60, 10 },
	{ 36.0f, 710, 660, 36.0f, 430, 4.35f, 4.35f, 3150, 2500, 2000, 2500, 4100, 72, 10 },
	{ 37.0f, 720, 670, 37.0f, 435, 4.40f, 4.40f, 3150, 2500, 2000, 2500, 4200, 72, 10 },
	{ 38.0f, 730, 680, 38.0f, 440, 4.45f, 4.45f, 4000, 3150, 2500, 3150, 4300, 72, 11 },
	{ 39.0f, 740, 690, 39.0f, 445, 4.50f, 4.50f, 4000, 3150, 2500, 3150, 4400, 72, 11 },
	{ 40.0f, 750, 700, 40.0f, 450, 4.55f, 4.55f, 4000, 3150, 2500, 3150, 4500, 84, 11 },
	{ 41.0f, 760, 710, 50.0f, 455, 4.60f, 4.60f, 4000, 3150, 2500, 3150, 4600, 84, 11 },
	{ 42.0f, 770, 720, 60.0f, 460, 4.65f, 4.65f, 4000, 3150, 2500, 3150, 4700, 84, 11 },
	{ 43.0f, 780, 730, 70.0f, 465, 4.70f, 4.70f, 4000, 3150, 2500, 3150, 4800, 84, 11 },
	{ 44.0f, 790, 740, 80.0f, 470, 4.75f, 4.75f, 4000, 3150, 2500, 3150, 4900, 96, 11 },
	{ 45.0f, 800, 750, 90.0f, 475, 4.80f, 4.80f, 4000, 3150, 2500, 3150, 5000, 96, 11 },
	{ 46.0f, 810, 760, 100.f, 480, 4.85f, 4.85f, 5000, 4000, 3150, 4000, 5100, 96, 12 },
	{ 47.0f, 820, 770, 110.f, 485, 4.90f, 4.90f, 5000, 4000, 3150, 4000, 5200, 96, 12 },
	{ 48.0f, 830, 780, 120.f, 490, 4.95f, 4.95f, 5000, 4000, 3150, 4000, 5300, 108, 12 },
	{ 49.0f, 840, 790, 130.f, 495, 5.00f, 5.00f, 5000, 4000, 3150, 4000, 5400, 108, 12 },
	{ 50.0f, 850, 800, 140.f, 500, 5.10f, 5.05f, 5000, 4000, 3150, 4000, 5500, 108, 12 },
	{ 52.0f, 860, 810, 150.f, 505, 5.20f, 5.10f, 5000, 4000, 3150, 4000, 5600, 108, 12 },
	{ 54.0f, 870, 820, 160.f, 510, 5.30f, 5.15f, 5000, 4000, 3150, 4000, 5700, 120, 12 },
	{ 56.0f, 880, 830, 170.f, 515, 5.40f, 5.20f, 5000, 4000, 3150, 4000, 5800, 120, 12 },
	{ 58.0f, 890, 840, 180.f, 520, 5.50f, 5.25f, 6300, 5000, 4000, 5000, 5900, 120, 13 },
	{ 60.0f, 900, 850, 190.f, 525, 5.60f, 5.30f, 6300, 5000, 4000, 5000, 6000, 120, 13 },
	{ 62.0f, 910, 860, 200.f, 530, 5.70f, 5.35f, 6300, 5000, 4000, 5000, 6100, 132, 13 },
	{ 64.0f, 920, 870, 210.f, 535, 5.80f, 5.40f, 6300, 5000, 4000, 5000, 6200, 132, 13 },
	{ 66.0f, 930, 880, 220.f, 540, 5.90f, 5.45f, 6300, 5000, 4000, 5000, 6300, 132, 13 },
	{ 68.0f, 940, 890, 230.f, 545, 6.00f, 5.50f, 6300, 5000, 4000, 5000, 6400, 132, 13 },
	{ 70.0f, 950, 900, 240.f, 550, 6.10f, 5.55f, 6300, 5000, 4000, 5000, 6500, 144, 13 },
	{ 72.0f, 960, 910, 250.f, 555, 6.20f, 5.60f, 6300, 5000, 4000, 5000, 6600, 144, 13 },
	{ 74.0f, 970, 920, 260.f, 560, 6.30f, 5.65f, 8000, 6300, 5000, 6300, 6700, 144, 14 },
	{ 76.0f, 980, 930, 270.f, 565, 6.40f, 5.70f, 8000, 6300, 5000, 6300, 6800, 144, 14 },
	{ 78.0f, 990, 940, 280.f, 570, 6.50f, 5.75f, 8000, 6300, 5000, 6300, 6900, 156, 14 },
	{ 80.0f, 1000, 950, 290.f, 575, 6.60f, 5.80f, 8000, 6300, 5000, 6300, 7000, 156, 14 },
	{ 82.0f, 1000, 960, 300.f, 580, 6.70f, 5.85f, 8000, 6300, 5000, 6300, 7100, 156, 14 },
	{ 84.0f, 1000, 970, 320.f, 585, 6.80f, 5.90f, 8000, 6300, 5000, 6300, 7200, 156, 14 },
	{ 86.0f, 1000, 980, 340.f, 590, 6.90f, 5.95f, 8000, 6300, 5000, 6300, 7300, 168, 14 },
	{ 88.0f, 1000, 990, 360.f, 595, 7.00f, 6.00f, 8000, 6300, 5000, 6300, 7400, 168, 14 },
	{ 90.0f, 1000, 1000, 380.f, 600, 7.50f, 6.05f, 13500, 8000, 6300, 13500, 7500, 168, 15 },
	{ 92.0f, 1000, 1000, 400.f, 605, 8.00f, 6.10f, 13500, 8000, 6300, 13500, 7600, 168, 15 },
	{ 94.0f, 1000, 1000, 420.f, 610, 8.50f, 6.15f, 13500, 8000, 6300, 13500, 7700, -180, 15 },
	{ 96.0f, 1000, 1000, 440.f, 615, 9.00f, 6.20f, 13500, 8000, 6300, 13500, 7800, -180, 15 },
	{ 98.0f, 1000, 1000, 460.f, 620, 9.50f, 6.25f, 13500, 8000, 6300, 13500, 7900, -180, 15 },
	{ 100.f, 1000, 1000, 480.f, 625, 10.0f, 6.30f, 13500, 8000, 6300, 13500, 8000, -180, 15 },
	{ 100.f, 1000, 1000, 500.f, 630, 10.0f, 6.35f, 13500, 8000, 6300, 13500, 8000, -180, 15 },
	{ 100.f, 1000, 1000, 500.f, 635, 10.0f, 6.40f, 13500, 8000, 6300, 13500, 8000, -180, 15 },
};

static inline float ivc_rate1(int v) {
	return IVC[v & 127][5];
}
static inline float ivc_manual(int v) {
	return IVC[v & 127][11];
}
static inline float ivc_eq_freq(int v) {
	return IVC[v & 127][9];
}

/* ── Biquad helpers ──────────────────────────────────────────────────────── */

static inline double biquad_process(SS_Biquad *c, SS_BiquadState *s, double x) {
	double y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
	s->x2 = s->x1;
	s->x1 = x;
	s->y2 = s->y1;
	s->y1 = y;
	return y;
}

static void biquad_zero(SS_BiquadState *s) {
	s->x1 = s->x2 = s->y1 = s->y2 = 0;
}

/* Identity passthrough coefficients */
static void biquad_identity(SS_Biquad *c) {
	c->b0 = 1;
	c->b1 = 0;
	c->b2 = 0;
	c->a1 = 0;
	c->a2 = 0;
}

/* Apply low shelf then high shelf inline */
static inline double apply_shelves(double x,
                                   SS_Biquad *lc, SS_BiquadState *ls,
                                   SS_Biquad *hc, SS_BiquadState *hs) {
	double l = lc->b0 * x + lc->b1 * ls->x1 + lc->b2 * ls->x2 - lc->a1 * ls->y1 - lc->a2 * ls->y2;
	ls->x2 = ls->x1;
	ls->x1 = x;
	ls->y2 = ls->y1;
	ls->y1 = l;
	double h = hc->b0 * l + hc->b1 * hs->x1 + hc->b2 * hs->x2 - hc->a1 * hs->y1 - hc->a2 * hs->y2;
	hs->x2 = hs->x1;
	hs->x1 = l;
	hs->y2 = hs->y1;
	hs->y1 = h;
	return h;
}

/* Robert Bristow-Johnson shelf (S=1) */
static void compute_shelf(SS_Biquad *c, double db_gain, double f0, double fs, int is_low) {
	double A = pow(10.0, db_gain / 40.0);
	double w0 = 2.0 * M_PI * f0 / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = (sw / 2.0) * sqrt((A + 1.0 / A) * (1.0 / 1.0 - 1) + 2.0);
	double b0, b1, b2, a0, a1, a2;
	double sA = sqrt(A);
	if(is_low) {
		b0 = A * (A + 1 - (A - 1) * cw + 2 * sA * alpha);
		b1 = 2 * A * (A - 1 - (A + 1) * cw);
		b2 = A * (A + 1 - (A - 1) * cw - 2 * sA * alpha);
		a0 = A + 1 + (A - 1) * cw + 2 * sA * alpha;
		a1 = -2 * (A - 1 + (A + 1) * cw);
		a2 = A + 1 + (A - 1) * cw - 2 * sA * alpha;
	} else {
		b0 = A * (A + 1 + (A - 1) * cw + 2 * sA * alpha);
		b1 = -2 * A * (A - 1 + (A + 1) * cw);
		b2 = A * (A + 1 + (A - 1) * cw - 2 * sA * alpha);
		a0 = A + 1 - (A - 1) * cw + 2 * sA * alpha;
		a1 = 2 * (A - 1 - (A + 1) * cw);
		a2 = A + 1 - (A - 1) * cw - 2 * sA * alpha;
	}
	c->b0 = b0 / a0;
	c->b1 = b1 / a0;
	c->b2 = b2 / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Peaking EQ (used by StereoEQ mid bands) */
static void compute_peaking_eq(SS_Biquad *c, double freq, double gain_db, double Q, double fs) {
	double A = pow(10.0, gain_db / 40.0);
	double w0 = 2.0 * M_PI * freq / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = sw / (2.0 * Q);
	double b0 = 1 + alpha * A;
	double b1 = -2 * cw;
	double b2 = 1 - alpha * A;
	double a0 = 1 + alpha / A;
	double a1 = -2 * cw;
	double a2 = 1 - alpha / A;
	c->b0 = b0 / a0;
	c->b1 = b1 / a0;
	c->b2 = b2 / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Low shelf used by StereoEQ (same formula but standalone) */
static void compute_low_shelf(SS_Biquad *c, double freq, double gain_db, double fs) {
	compute_shelf(c, gain_db, freq, fs, 1);
}

static void compute_high_shelf(SS_Biquad *c, double freq, double gain_db, double fs) {
	compute_shelf(c, gain_db, freq, fs, 0);
}

/* Standard 2nd-order lowpass */
static void compute_lpf(SS_Biquad *c, double freq, double Q, double fs) {
	double w0 = 2.0 * M_PI * freq / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = sw / (2.0 * Q);
	double b1v = 1.0 - cw;
	double b0v = b1v * 0.5;
	double a0 = 1.0 + alpha;
	double a1 = -2.0 * cw;
	double a2 = 1.0 - alpha;
	c->b0 = b0v / a0;
	c->b1 = b1v / a0;
	c->b2 = b0v / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Standard 2nd-order highpass */
static void compute_hpf(SS_Biquad *c, double freq, double Q, double fs) {
	double w0 = 2.0 * M_PI * freq / fs;
	double cw = cos(w0), sw = sin(w0);
	double alpha = sw / (2.0 * Q);
	double b0v = (1.0 + cw) * 0.5;
	double b1v = -(1.0 + cw);
	double a0 = 1.0 + alpha;
	double a1 = -2.0 * cw;
	double a2 = 1.0 - alpha;
	c->b0 = b0v / a0;
	c->b1 = b1v / a0;
	c->b2 = b0v / a0;
	c->a1 = a1 / a0;
	c->a2 = a2 / a0;
}

/* Pan lookup tables (128 entries, index = pan+64, range -64..63) */
static float pan_table_left[128];
static float pan_table_right[128];
static int pan_tables_initialized = 0;

static void init_pan_tables(void) {
	if(pan_tables_initialized) return;
	for(int pan = -64; pan <= 63; pan++) {
		float real_pan = (float)(pan + 64) / 127.0f;
		int idx = pan + 64;
		pan_table_left[idx] = cosf((float)(M_PI * 0.5) * real_pan);
		pan_table_right[idx] = sinf((float)(M_PI * 0.5) * real_pan);
	}
	pan_tables_initialized = 1;
}

/* LFO helpers (matching TS waveforms) */
static inline float lfo_triangle(float phase) {
	return 1.0f - 4.0f * fabsf(phase - 0.5f);
}
static inline float lfo_square(float phase) {
	return phase > 0.5f ? -1.0f : -(float)cos((phase - 0.75f) * 2.0 * M_PI);
}
static inline float lfo_sine(float phase) {
	return sinf(2.0f * (float)M_PI * phase);
}
static inline float lfo_saw1(float phase) {
	return 1.0f - 2.0f * phase;
}
static inline float lfo_saw2(float phase) {
	return 2.0f * phase - 1.0f;
}

static float compute_lfo(int wave, float phase) {
	switch(wave) {
		default:
			return lfo_triangle(phase);
		case 1:
			return lfo_square(phase);
		case 2:
			return lfo_sine(phase);
		case 3:
			return lfo_saw1(phase);
		case 4:
			return lfo_saw2(phase);
	}
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 1.  Thru (0x0000)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
	SS_InsertionProcessor base;
} SS_ThruFX;

static void thru_process(SS_InsertionProcessor *self,
                         const float *iL, const float *iR,
                         float *oL, float *oR,
                         float *oRev, float *oCho, float *oDel,
                         int start, int n) {
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;
	for(int i = 0; i < n; i++) {
		float sL = iL[i], sR = iR[i];
		oL[start + i] += sL;
		oR[start + i] += sR;
		float mono = (sL + sR) * 0.5f;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
}
static void thru_set_param(SS_InsertionProcessor *self, int p, int v) {
	(void)self;
	(void)p;
	(void)v;
}
static void thru_reset(SS_InsertionProcessor *self) {
	(void)self;
}
static void thru_free(SS_InsertionProcessor *self) {
	free(self);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2.  StereoEQ (0x0100)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	float level;
	float low_freq, low_gain;
	float hi_freq, hi_gain;
	float m1_freq, m1_gain;
	int m1_q_idx;
	float m2_freq, m2_gain;
	int m2_q_idx;
	SS_Biquad low_c, m1_c, m2_c, hi_c;
	SS_BiquadState low_l, low_r, m1_l, m1_r, m2_l, m2_r, hi_l, hi_r;
} SS_StereoEQFX;

static const float EQ_Q_TABLE[5] = { 0.5f, 1.0f, 2.0f, 4.0f, 9.0f };

static void seq_update(SS_StereoEQFX *e) {
	double fs = e->sample_rate;
	compute_low_shelf(&e->low_c, e->low_freq, e->low_gain * 0.5, fs);
	compute_peaking_eq(&e->m1_c, e->m1_freq, e->m1_gain, EQ_Q_TABLE[e->m1_q_idx], fs);
	compute_peaking_eq(&e->m2_c, e->m2_freq, e->m2_gain, EQ_Q_TABLE[e->m2_q_idx], fs);
	compute_high_shelf(&e->hi_c, e->hi_freq, e->hi_gain * 0.5, fs);
}

static void seq_process(SS_InsertionProcessor *self,
                        const float *iL, const float *iR,
                        float *oL, float *oR,
                        float *oRev, float *oCho, float *oDel,
                        int start, int n) {
	SS_StereoEQFX *e = (SS_StereoEQFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;
	float level = e->level;
	for(int i = 0; i < n; i++) {
		double sL = iL[i], sR = iR[i];
		sL = biquad_process(&e->low_c, &e->low_l, sL);
		sR = biquad_process(&e->low_c, &e->low_r, sR);
		sL = biquad_process(&e->m1_c, &e->m1_l, sL);
		sR = biquad_process(&e->m1_c, &e->m1_r, sR);
		sL = biquad_process(&e->m2_c, &e->m2_l, sL);
		sR = biquad_process(&e->m2_c, &e->m2_r, sR);
		sL = biquad_process(&e->hi_c, &e->hi_l, sL);
		sR = biquad_process(&e->hi_c, &e->hi_r, sR);
		oL[start + i] += (float)sL * level;
		oR[start + i] += (float)sR * level;
		float mono = 0.5f * ((float)sL + (float)sR);
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
}

static void seq_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_StereoEQFX *e = (SS_StereoEQFX *)self;
	switch(p) {
		case 0x03:
			e->low_freq = v == 1 ? 400.0f : 200.0f;
			break;
		case 0x04:
			e->low_gain = (float)(v - 64);
			break;
		case 0x05:
			e->hi_freq = v == 1 ? 8000.0f : 4000.0f;
			break;
		case 0x06:
			e->hi_gain = (float)(v - 64);
			break;
		case 0x07:
			e->m1_freq = ivc_eq_freq(v);
			break;
		case 0x08:
			e->m1_q_idx = (v < 5 ? v : 1);
			break;
		case 0x09:
			e->m1_gain = (float)(v - 64);
			break;
		case 0x0a:
			e->m2_freq = ivc_eq_freq(v);
			break;
		case 0x0b:
			e->m2_q_idx = (v < 5 ? v : 1);
			break;
		case 0x0c:
			e->m2_gain = (float)(v - 64);
			break;
		case 0x16:
			e->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	seq_update(e);
}

static void seq_reset(SS_InsertionProcessor *self) {
	SS_StereoEQFX *e = (SS_StereoEQFX *)self;
	e->level = 1.0f;
	e->low_freq = 400.0f;
	e->low_gain = 5.0f;
	e->hi_freq = 8000.0f;
	e->hi_gain = -12.0f;
	e->m1_freq = 1600.0f;
	e->m1_gain = 8.0f;
	e->m1_q_idx = 0;
	e->m2_freq = 1000.0f;
	e->m2_gain = -8.0f;
	e->m2_q_idx = 0;
	biquad_zero(&e->low_l);
	biquad_zero(&e->low_r);
	biquad_zero(&e->m1_l);
	biquad_zero(&e->m1_r);
	biquad_zero(&e->m2_l);
	biquad_zero(&e->m2_r);
	biquad_zero(&e->hi_l);
	biquad_zero(&e->hi_r);
	seq_update(e);
}
static void seq_free(SS_InsertionProcessor *self) {
	free(self);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 3.  Phaser (0x0120)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PHASER_STAGES 8

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	float manual, manual_offset;
	float rate, depth, reso, mix, low_gain, hi_gain, level;
	float phase;
	float prev_l, prev_r;
	float prev_in_l[PHASER_STAGES], prev_out_l[PHASER_STAGES];
	float prev_in_r[PHASER_STAGES], prev_out_r[PHASER_STAGES];
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_l, ls_r, hs_l, hs_r;
} SS_PhaserFX;

static void phaser_set_manual(SS_PhaserFX *p, float m) {
	if(m > 1000.0f) {
		p->manual_offset = 600.0f * 1.5f * 4.0f;
		p->manual = m;
	} else {
		p->manual_offset = 600.0f;
		p->manual = m * 4.0f;
	}
}

static void phaser_update_shelves(SS_PhaserFX *p) {
	compute_shelf(&p->ls_c, p->low_gain, 200.0, p->sample_rate, 1);
	compute_shelf(&p->hs_c, p->hi_gain, 4000.0, p->sample_rate, 0);
}

static void phaser_process(SS_InsertionProcessor *self,
                           const float *iL, const float *iR,
                           float *oL, float *oR,
                           float *oRev, float *oCho, float *oDel,
                           int start, int n) {
	SS_PhaserFX *p = (SS_PhaserFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;
	float rate_inc = p->rate / (float)p->sample_rate;
	float fb = p->reso * 0.9f;
	float phase = p->phase;
	float prevL = p->prev_l, prevR = p->prev_r;
	for(int i = 0; i < n; i++) {
		double sL = apply_shelves(iL[i], &p->ls_c, &p->ls_l, &p->hs_c, &p->hs_l);
		double sR = apply_shelves(iR[i], &p->ls_c, &p->ls_r, &p->hs_c, &p->hs_r);

		/* Triangle LFO (TS: 2*|phase-0.5|, lfoMul = 1 - depth*lfo) */
		float lfo = 2.0f * fabsf(phase - 0.5f);
		if((phase += rate_inc) >= 1.0f) phase -= 1.0f;
		float lfoMul = 1.0f - p->depth * lfo;
		float fc = p->manual_offset + p->manual * lfoMul;

		double tanT = tan(M_PI * fc / p->sample_rate);
		double a = (1.0 - tanT) / (1.0 + tanT);
		if(a < -0.9999) a = -0.9999;
		if(a > 0.9999) a = 0.9999;

		double apL = sL + fb * prevL;
		double apR = sR + fb * prevR;
		for(int s = 0; s < PHASER_STAGES; s++) {
			double outL = -a * apL + p->prev_in_l[s] + a * p->prev_out_l[s];
			p->prev_in_l[s] = (float)apL;
			p->prev_out_l[s] = (float)outL;
			apL = outL;
			double outR = -a * apR + p->prev_in_r[s] + a * p->prev_out_r[s];
			p->prev_in_r[s] = (float)apR;
			p->prev_out_r[s] = (float)outR;
			apR = outR;
		}
		prevL = (float)apL;
		prevR = (float)apR;

		float outL = ((float)sL + (float)apL * p->mix) * p->level;
		float outR = ((float)sR + (float)apR * p->mix) * p->level;
		oL[start + i] += outL;
		oR[start + i] += outR;
		float mono = (outL + outR) * 0.5f;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	p->phase = phase;
	p->prev_l = prevL;
	p->prev_r = prevR;
}

static void phaser_set_param(SS_InsertionProcessor *self, int param, int v) {
	SS_PhaserFX *p = (SS_PhaserFX *)self;
	switch(param) {
		case 0x03:
			phaser_set_manual(p, ivc_manual(v));
			break;
		case 0x04:
			p->rate = ivc_rate1(v);
			break;
		case 0x05:
			p->depth = (float)v / 128.0f;
			break;
		case 0x06:
			p->reso = (float)v / 127.0f;
			break;
		case 0x07:
			p->mix = (float)v / 127.0f;
			break;
		case 0x13:
			p->low_gain = (float)(v - 64);
			break;
		case 0x14:
			p->hi_gain = (float)(v - 64);
			break;
		case 0x16:
			p->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	phaser_update_shelves(p);
}

static void phaser_reset(SS_InsertionProcessor *self) {
	SS_PhaserFX *p = (SS_PhaserFX *)self;
	p->phase = 0.35f;
	phaser_set_manual(p, 620.0f);
	p->rate = 0.85f;
	p->depth = 64.0f / 128.0f;
	p->reso = 16.0f / 127.0f;
	p->mix = 1.0f;
	p->low_gain = 0;
	p->hi_gain = 0;
	p->level = 104.0f / 127.0f;
	p->prev_l = p->prev_r = 0;
	memset(p->prev_in_l, 0, sizeof(p->prev_in_l));
	memset(p->prev_out_l, 0, sizeof(p->prev_out_l));
	memset(p->prev_in_r, 0, sizeof(p->prev_in_r));
	memset(p->prev_out_r, 0, sizeof(p->prev_out_r));
	biquad_zero(&p->ls_l);
	biquad_zero(&p->ls_r);
	biquad_zero(&p->hs_l);
	biquad_zero(&p->hs_r);
	phaser_update_shelves(p);
}
static void phaser_free(SS_InsertionProcessor *self) {
	free(self);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4.  AutoPan (0x0126)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AUTOPAN_GAIN_LVL 0.935f
#define AUTOPAN_LEVEL_EXP 2.0f
#define AUTOPAN_PAN_SMOOTH 0.01f

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	int mod_wave;
	float mod_rate, mod_depth, low_gain, hi_gain, level;
	float phase, current_pan;
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_l, ls_r, hs_l, hs_r;
} SS_AutoPanFX;

static void autopan_update_shelves(SS_AutoPanFX *e) {
	compute_shelf(&e->ls_c, e->low_gain, 200.0, e->sample_rate, 1);
	compute_shelf(&e->hs_c, e->hi_gain, 4000.0, e->sample_rate, 0);
}

static void autopan_process(SS_InsertionProcessor *self,
                            const float *iL, const float *iR,
                            float *oL, float *oR,
                            float *oRev, float *oCho, float *oDel,
                            int start, int n) {
	SS_AutoPanFX *e = (SS_AutoPanFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;
	float depth = powf(e->mod_depth / 127.0f, AUTOPAN_LEVEL_EXP);
	float scale = (2.0f / (1.0f + depth)) * AUTOPAN_GAIN_LVL;
	float rate_inc = e->mod_rate / (float)e->sample_rate;
	float phase = e->phase, cur_pan = e->current_pan;
	for(int i = 0; i < n; i++) {
		double sL = apply_shelves(iL[i], &e->ls_c, &e->ls_l, &e->hs_c, &e->hs_l);
		double sR = apply_shelves(iR[i], &e->ls_c, &e->ls_r, &e->hs_c, &e->hs_r);

		float lfo = compute_lfo(e->mod_wave, phase);
		if((phase += rate_inc) >= 1.0f) phase -= 1.0f;
		cur_pan += (lfo - cur_pan) * AUTOPAN_PAN_SMOOTH;
		float pan = cur_pan * depth;
		float gainL = (1.0f - pan) * 0.5f * scale;
		float gainR = (1.0f + pan) * 0.5f * scale;

		float outL = (float)sL * e->level * gainL;
		float outR = (float)sR * e->level * gainR;
		oL[start + i] += outL;
		oR[start + i] += outR;
		float mono = (outL + outR) * 0.5f;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	e->phase = phase;
	e->current_pan = cur_pan;
}

static void autopan_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_AutoPanFX *e = (SS_AutoPanFX *)self;
	switch(p) {
		case 0x03:
			e->mod_wave = v;
			break;
		case 0x04:
			e->mod_rate = ivc_rate1(v);
			break;
		case 0x05:
			e->mod_depth = (float)v;
			break;
		case 0x13:
			e->low_gain = (float)(v - 64);
			break;
		case 0x14:
			e->hi_gain = (float)(v - 64);
			break;
		case 0x16:
			e->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	autopan_update_shelves(e);
}

static void autopan_reset(SS_InsertionProcessor *self) {
	SS_AutoPanFX *e = (SS_AutoPanFX *)self;
	e->mod_wave = 1;
	e->mod_rate = 3.05f;
	e->mod_depth = 96.0f;
	e->low_gain = 0;
	e->hi_gain = 0;
	e->level = 1.0f;
	e->phase = 0;
	e->current_pan = 0;
	biquad_zero(&e->ls_l);
	biquad_zero(&e->ls_r);
	biquad_zero(&e->hs_l);
	biquad_zero(&e->hs_r);
	autopan_update_shelves(e);
}
static void autopan_free(SS_InsertionProcessor *self) {
	free(self);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5.  Tremolo (0x0125)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TREMOLO_GAIN_SMOOTH 0.01f

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	int mod_wave;
	float mod_rate, mod_depth, low_gain, hi_gain, level;
	float phase, current_gain;
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_l, ls_r, hs_l, hs_r;
} SS_TremoloFX;

static void tremolo_update_shelves(SS_TremoloFX *e) {
	compute_shelf(&e->ls_c, e->low_gain, 200.0, e->sample_rate, 1);
	compute_shelf(&e->hs_c, e->hi_gain, 4000.0, e->sample_rate, 0);
}

static void tremolo_process(SS_InsertionProcessor *self,
                            const float *iL, const float *iR,
                            float *oL, float *oR,
                            float *oRev, float *oCho, float *oDel,
                            int start, int n) {
	SS_TremoloFX *e = (SS_TremoloFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;
	float rate_inc = e->mod_rate / (float)e->sample_rate;
	float phase = e->phase, cur_gain = e->current_gain;
	for(int i = 0; i < n; i++) {
		double sL = apply_shelves(iL[i], &e->ls_c, &e->ls_l, &e->hs_c, &e->hs_l);
		double sR = apply_shelves(iR[i], &e->ls_c, &e->ls_r, &e->hs_c, &e->hs_r);

		float lfo = compute_lfo(e->mod_wave, phase);
		if((phase += rate_inc) >= 1.0f) phase -= 1.0f;

		float trem_level = 1.0f - (lfo * 0.5f + 0.5f) * (e->mod_depth / 127.0f);
		cur_gain += (trem_level - cur_gain) * TREMOLO_GAIN_SMOOTH;

		float outL = (float)sL * e->level * cur_gain;
		float outR = (float)sR * e->level * cur_gain;
		oL[start + i] += outL;
		oR[start + i] += outR;
		float mono = (outL + outR) * 0.5f;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	e->phase = phase;
	e->current_gain = cur_gain;
}

static void tremolo_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_TremoloFX *e = (SS_TremoloFX *)self;
	switch(p) {
		case 0x03:
			e->mod_wave = v;
			break;
		case 0x04:
			e->mod_rate = ivc_rate1(v);
			break;
		case 0x05:
			e->mod_depth = (float)v;
			break;
		case 0x13:
			e->low_gain = (float)(v - 64);
			break;
		case 0x14:
			e->hi_gain = (float)(v - 64);
			break;
		case 0x16:
			e->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	tremolo_update_shelves(e);
}

static void tremolo_reset(SS_InsertionProcessor *self) {
	SS_TremoloFX *e = (SS_TremoloFX *)self;
	e->mod_wave = 1;
	e->mod_rate = 3.05f;
	e->mod_depth = 96.0f;
	e->low_gain = 0;
	e->hi_gain = 0;
	e->level = 1.0f;
	e->phase = 0;
	e->current_gain = 1.0f;
	biquad_zero(&e->ls_l);
	biquad_zero(&e->ls_r);
	biquad_zero(&e->hs_l);
	biquad_zero(&e->hs_r);
	tremolo_update_shelves(e);
}
static void tremolo_free(SS_InsertionProcessor *self) {
	free(self);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 6.  AutoWah (0x0121)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AW_SENS_COEFF 27.0f
#define AW_PEAK_DB 28.0f
#define AW_HPF_Q_DB -28.0f
#define AW_HPF_FC 400.0f
#define AW_MANUAL_SCALE 0.62f
#define AW_FC_SMOOTH 0.005f
#define AW_DEPTH_MUL 5.0f
#define AW_LFO_SMOOTH (AW_DEPTH_MUL * 0.5f)

typedef struct {
	SS_InsertionProcessor base;
	double sample_rate;
	int fil_type, polarity;
	float sens, manual, peak, rate, depth, pan, low_gain, hi_gain, level;
	float phase, last_fc, envelope;
	double attack_coeff, release_coeff;
	SS_Biquad coeffs, hp_coeffs;
	SS_BiquadState state, hp_state;
	SS_Biquad ls_c, hs_c;
	SS_BiquadState ls_s, hs_s;
} SS_AutoWahFX;

static void aw_set_manual(SS_AutoWahFX *e, int value) {
	float target = value * AW_MANUAL_SCALE;
	int fl = (int)target, cl = fl + 1;
	if(fl < 0) fl = 0;
	if(fl > 127) fl = 127;
	if(cl > 127) cl = 127;
	float frac = target - fl;
	e->manual = ivc_manual(fl) + (ivc_manual(cl) - ivc_manual(fl)) * frac;
}

static void aw_update_shelves(SS_AutoWahFX *e) {
	compute_shelf(&e->ls_c, e->low_gain, 200.0, e->sample_rate, 1);
	compute_shelf(&e->hs_c, e->hi_gain, 4000.0, e->sample_rate, 0);
}

static void aw_process(SS_InsertionProcessor *self,
                       const float *iL, const float *iR,
                       float *oL, float *oR,
                       float *oRev, float *oCho, float *oDel,
                       int start, int n) {
	SS_AutoWahFX *e = (SS_AutoWahFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;

	float rate_inc = e->rate / (float)e->sample_rate;
	float peak = powf(10.0f, (e->peak / 127.0f * AW_PEAK_DB) / 20.0f);
	float hpf_peak = powf(10.0f, (e->peak / 127.0f * AW_HPF_Q_DB) / 20.0f);
	float pol = (e->polarity == 0) ? -1.0f : AW_DEPTH_MUL;
	float depth = (e->depth / 127.0f) * pol;
	float sens = e->sens / 127.0f;

	int pan_idx = (int)(e->pan + 64);
	if(pan_idx < 0) pan_idx = 0;
	if(pan_idx > 127) pan_idx = 127;
	float gainL = pan_table_left[pan_idx];
	float gainR = pan_table_right[pan_idx];

	float phase = e->phase;
	float last_fc = e->last_fc;
	double env = e->envelope;
	double atk = e->attack_coeff, rel = e->release_coeff;

	for(int i = 0; i < n; i++) {
		/* Mono: average L+R */
		double s = apply_shelves((iL[i] + iR[i]) * 0.5f,
		                         &e->ls_c, &e->ls_s,
		                         &e->hs_c, &e->hs_s);

		double rect = fabs(s);
		if(rect > env)
			env = atk * env + (1.0 - atk) * rect;
		else
			env = rel * env + (1.0 - rel) * rect;

		float lfo = 2.0f * fabsf(phase - 0.5f) * depth;
		if((phase += rate_inc) >= 1.0f) phase -= 1.0f;

		float lfo_mul;
		if(lfo >= AW_LFO_SMOOTH || pol < 0)
			lfo_mul = 1.0f;
		else
			lfo_mul = sinf(lfo * (float)M_PI / (2.0f * AW_LFO_SMOOTH));

		float base = e->manual * (1.0f + sens * (float)env * AW_SENS_COEFF);
		float fc = base * (1.0f + lfo_mul * lfo);
		if(fc < 20.0f) fc = 20.0f;
		float target = fc < 10.0f ? 10.0f : fc;
		last_fc += (target - last_fc) * AW_FC_SMOOTH;

		compute_lpf(&e->coeffs, last_fc, peak, e->sample_rate);

		double proc = s;
		if(e->fil_type == 1) {
			compute_hpf(&e->hp_coeffs, AW_HPF_FC, hpf_peak, e->sample_rate);
			proc = biquad_process(&e->hp_coeffs, &e->hp_state, proc);
		}
		float mono = (float)biquad_process(&e->coeffs, &e->state, proc) * e->level;

		oL[start + i] += mono * gainL;
		oR[start + i] += mono * gainR;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
	e->phase = phase;
	e->last_fc = last_fc;
	e->envelope = env;
}

static void aw_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_AutoWahFX *e = (SS_AutoWahFX *)self;
	switch(p) {
		case 0x03:
			e->fil_type = v;
			break;
		case 0x04:
			e->sens = (float)v;
			break;
		case 0x05:
			aw_set_manual(e, v);
			break;
		case 0x06:
			e->peak = (float)v;
			break;
		case 0x07:
			e->rate = ivc_rate1(v);
			break;
		case 0x08:
			e->depth = (float)v;
			break;
		case 0x09:
			e->polarity = v;
			break;
		case 0x13:
			e->low_gain = (float)(v - 64);
			break;
		case 0x14:
			e->hi_gain = (float)(v - 64);
			break;
		case 0x15:
			e->pan = (float)(v - 64);
			break;
		case 0x16:
			e->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
	aw_update_shelves(e);
}

static void aw_reset_impl(SS_AutoWahFX *e) {
	e->fil_type = 1;
	e->sens = 0;
	e->peak = 62;
	e->rate = 2.05f;
	e->depth = 72;
	e->polarity = 1;
	e->low_gain = 0;
	e->hi_gain = 0;
	e->pan = 0;
	e->level = 96.0f / 127.0f;
	e->phase = 0.2f;
	e->envelope = 0;
	aw_set_manual(e, 68);
	e->last_fc = e->manual;
	biquad_identity(&e->coeffs);
	biquad_zero(&e->state);
	biquad_identity(&e->hp_coeffs);
	biquad_zero(&e->hp_state);
	biquad_zero(&e->ls_s);
	biquad_zero(&e->hs_s);
	aw_update_shelves(e);
}

static void aw_reset(SS_InsertionProcessor *self) {
	aw_reset_impl((SS_AutoWahFX *)self);
}
static void aw_free(SS_InsertionProcessor *self) {
	free(self);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 7.  PhAutoWah (0x1108) — parallel Phaser + AutoWah
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
	SS_InsertionProcessor base;
	SS_PhaserFX phaser;
	SS_AutoWahFX auto_wah;
	float ph_pan; /* 0..127, index into pan_table */
	float aw_pan;
	float level;
	float *buf_ph;
	float *buf_aw;
	uint32_t buf_size;
} SS_PhAutoWahFX;

static void phaw_process(SS_InsertionProcessor *self,
                         const float *iL, const float *iR,
                         float *oL, float *oR,
                         float *oRev, float *oCho, float *oDel,
                         int start, int n) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	float rev = self->send_level_to_reverb;
	float cho = self->send_level_to_chorus;
	float del = self->send_level_to_delay;

	/* Process phaser (only left input) into buf_ph */
	memset(e->buf_ph, 0, sizeof(float) * n);
	phaser_process(&e->phaser.base, iL, iL,
	               e->buf_ph, e->buf_ph,
	               e->buf_ph, e->buf_ph, e->buf_ph, /* sends = 0, ignored */
	               0, n);

	/* Process auto-wah (only right input) into buf_aw */
	memset(e->buf_aw, 0, sizeof(float) * n);
	aw_process(&e->auto_wah.base, iR, iR,
	           e->buf_aw, e->buf_aw,
	           e->buf_aw, e->buf_aw, e->buf_aw,
	           0, n);

	int ph_idx = (int)e->ph_pan;
	if(ph_idx < 0) ph_idx = 0;
	if(ph_idx > 127) ph_idx = 127;
	int aw_idx = (int)e->aw_pan;
	if(aw_idx < 0) aw_idx = 0;
	if(aw_idx > 127) aw_idx = 127;
	float phL = pan_table_left[ph_idx], phR = pan_table_right[ph_idx];
	float awL = pan_table_left[aw_idx], awR = pan_table_right[aw_idx];

	for(int i = 0; i < n; i++) {
		/* Divide by 2: each processor mixed both L+R into one buffer */
		float out_ph = e->buf_ph[i] * 0.5f * e->level;
		float out_aw = e->buf_aw[i] * 0.5f * e->level;
		float outL = out_ph * phL + out_aw * awL;
		float outR = out_ph * phR + out_aw * awR;
		oL[start + i] += outL;
		oR[start + i] += outR;
		float mono = (outL + outR) * 0.5f;
		if(oRev) oRev[i] += mono * rev;
		if(oCho) oCho[i] += mono * cho;
		if(oDel) oDel[i] += mono * del;
	}
}

static void phaw_set_param(SS_InsertionProcessor *self, int p, int v) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	if(p >= 0x03 && p <= 0x07) {
		phaser_set_param(&e->phaser.base, p, v);
		return;
	}
	if(p >= 0x08 && p <= 0x0e) {
		aw_set_param(&e->auto_wah.base, p - 5, v);
		return;
	}
	switch(p) {
		case 0x12:
			e->ph_pan = (float)v;
			break;
		case 0x13:
			phaser_set_param(&e->phaser.base, 0x16, v);
			break;
		case 0x14:
			e->aw_pan = (float)v;
			break;
		case 0x15:
			aw_set_param(&e->auto_wah.base, 0x16, v);
			break;
		case 0x16:
			e->level = (float)v / 127.0f;
			break;
		default:
			break;
	}
}

static void phaw_reset(SS_InsertionProcessor *self) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	e->ph_pan = 0;
	e->aw_pan = 127;
	e->level = 1.0f;
	phaser_reset(&e->phaser.base);
	aw_reset(&e->auto_wah.base);
	/* Override sub-processor levels to full */
	phaser_set_param(&e->phaser.base, 0x16, 127);
	aw_set_param(&e->auto_wah.base, 0x16, 127);
}

static void phaw_free(SS_InsertionProcessor *self) {
	SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)self;
	free(e->buf_ph);
	free(e->buf_aw);
	free(e);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Factory
 * ═══════════════════════════════════════════════════════════════════════════ */

SS_InsertionProcessor *ss_insertion_create(uint32_t type,
                                           uint32_t sample_rate,
                                           uint32_t max_buf_size) {
	init_pan_tables();

	switch(type) {
		/* ── Thru ── */
		case 0x0000: {
			SS_ThruFX *e = (SS_ThruFX *)calloc(1, sizeof(SS_ThruFX));
			if(!e) return NULL;
			e->base.type = type;
			e->base.send_level_to_reverb = 40.0f / 127.0f;
			e->base.send_level_to_chorus = 0;
			e->base.send_level_to_delay = 0;
			e->base.process = thru_process;
			e->base.set_parameter = thru_set_param;
			e->base.reset = thru_reset;
			e->base.free = thru_free;
			return &e->base;
		}

		/* ── StereoEQ ── */
		case 0x0100: {
			SS_StereoEQFX *e = (SS_StereoEQFX *)calloc(1, sizeof(SS_StereoEQFX));
			if(!e) return NULL;
			e->base.type = type;
			e->base.send_level_to_reverb = 0;
			e->base.send_level_to_chorus = 0;
			e->base.send_level_to_delay = 0;
			e->base.process = seq_process;
			e->base.set_parameter = seq_set_param;
			e->base.reset = seq_reset;
			e->base.free = seq_free;
			e->sample_rate = (double)sample_rate;
			biquad_identity(&e->low_c);
			biquad_identity(&e->m1_c);
			biquad_identity(&e->m2_c);
			biquad_identity(&e->hi_c);
			seq_reset(&e->base);
			return &e->base;
		}

		/* ── Phaser ── */
		case 0x0120: {
			SS_PhaserFX *e = (SS_PhaserFX *)calloc(1, sizeof(SS_PhaserFX));
			if(!e) return NULL;
			e->base.type = type;
			e->base.send_level_to_reverb = 40.0f / 127.0f;
			e->base.send_level_to_chorus = 0;
			e->base.send_level_to_delay = 0;
			e->base.process = phaser_process;
			e->base.set_parameter = phaser_set_param;
			e->base.reset = phaser_reset;
			e->base.free = phaser_free;
			e->sample_rate = (double)sample_rate;
			biquad_identity(&e->ls_c);
			biquad_identity(&e->hs_c);
			phaser_reset(&e->base);
			return &e->base;
		}

		/* ── AutoWah ── */
		case 0x0121: {
			SS_AutoWahFX *e = (SS_AutoWahFX *)calloc(1, sizeof(SS_AutoWahFX));
			if(!e) return NULL;
			e->base.type = type;
			e->base.send_level_to_reverb = 40.0f / 127.0f;
			e->base.send_level_to_chorus = 0;
			e->base.send_level_to_delay = 0;
			e->base.process = aw_process;
			e->base.set_parameter = aw_set_param;
			e->base.reset = aw_reset;
			e->base.free = aw_free;
			e->sample_rate = (double)sample_rate;
			e->attack_coeff = exp(-1.0 / (0.1 * sample_rate));
			e->release_coeff = exp(-1.0 / (0.1 * sample_rate));
			biquad_identity(&e->coeffs);
			biquad_identity(&e->hp_coeffs);
			biquad_identity(&e->ls_c);
			biquad_identity(&e->hs_c);
			aw_reset_impl(e);
			return &e->base;
		}

		/* ── Tremolo ── */
		case 0x0125: {
			SS_TremoloFX *e = (SS_TremoloFX *)calloc(1, sizeof(SS_TremoloFX));
			if(!e) return NULL;
			e->base.type = type;
			e->base.send_level_to_reverb = 40.0f / 127.0f;
			e->base.send_level_to_chorus = 0;
			e->base.send_level_to_delay = 0;
			e->base.process = tremolo_process;
			e->base.set_parameter = tremolo_set_param;
			e->base.reset = tremolo_reset;
			e->base.free = tremolo_free;
			e->sample_rate = (double)sample_rate;
			biquad_identity(&e->ls_c);
			biquad_identity(&e->hs_c);
			tremolo_reset(&e->base);
			return &e->base;
		}

		/* ── AutoPan ── */
		case 0x0126: {
			SS_AutoPanFX *e = (SS_AutoPanFX *)calloc(1, sizeof(SS_AutoPanFX));
			if(!e) return NULL;
			e->base.type = type;
			e->base.send_level_to_reverb = 40.0f / 127.0f;
			e->base.send_level_to_chorus = 0;
			e->base.send_level_to_delay = 0;
			e->base.process = autopan_process;
			e->base.set_parameter = autopan_set_param;
			e->base.reset = autopan_reset;
			e->base.free = autopan_free;
			e->sample_rate = (double)sample_rate;
			biquad_identity(&e->ls_c);
			biquad_identity(&e->hs_c);
			autopan_reset(&e->base);
			return &e->base;
		}

		/* ── PhAutoWah ── */
		case 0x1108: {
			SS_PhAutoWahFX *e = (SS_PhAutoWahFX *)calloc(1, sizeof(SS_PhAutoWahFX));
			if(!e) return NULL;
			if(max_buf_size == 0) max_buf_size = 512;
			e->buf_ph = (float *)calloc(max_buf_size, sizeof(float));
			e->buf_aw = (float *)calloc(max_buf_size, sizeof(float));
			if(!e->buf_ph || !e->buf_aw) {
				free(e->buf_ph);
				free(e->buf_aw);
				free(e);
				return NULL;
			}
			e->buf_size = max_buf_size;

			e->base.type = type;
			e->base.send_level_to_reverb = 40.0f / 127.0f;
			e->base.send_level_to_chorus = 0;
			e->base.send_level_to_delay = 0;
			e->base.process = phaw_process;
			e->base.set_parameter = phaw_set_param;
			e->base.reset = phaw_reset;
			e->base.free = phaw_free;

			/* Initialize embedded phaser */
			e->phaser.sample_rate = (double)sample_rate;
			e->phaser.base.type = 0x0120;
			e->phaser.base.send_level_to_reverb = 0;
			e->phaser.base.send_level_to_chorus = 0;
			e->phaser.base.send_level_to_delay = 0;
			e->phaser.base.process = phaser_process;
			e->phaser.base.set_parameter = phaser_set_param;
			e->phaser.base.reset = phaser_reset;
			e->phaser.base.free = NULL; /* owned by parent */
			biquad_identity(&e->phaser.ls_c);
			biquad_identity(&e->phaser.hs_c);
			phaser_reset(&e->phaser.base);

			/* Initialize embedded auto-wah */
			e->auto_wah.sample_rate = (double)sample_rate;
			e->auto_wah.base.type = 0x0121;
			e->auto_wah.base.send_level_to_reverb = 0;
			e->auto_wah.base.send_level_to_chorus = 0;
			e->auto_wah.base.send_level_to_delay = 0;
			e->auto_wah.base.process = aw_process;
			e->auto_wah.base.set_parameter = aw_set_param;
			e->auto_wah.base.reset = aw_reset;
			e->auto_wah.base.free = NULL;
			e->auto_wah.attack_coeff = exp(-1.0 / (0.1 * sample_rate));
			e->auto_wah.release_coeff = exp(-1.0 / (0.1 * sample_rate));
			biquad_identity(&e->auto_wah.coeffs);
			biquad_identity(&e->auto_wah.hp_coeffs);
			biquad_identity(&e->auto_wah.ls_c);
			biquad_identity(&e->auto_wah.hs_c);
			aw_reset_impl(&e->auto_wah);

			phaw_reset(&e->base);
			return &e->base;
		}

		default:
			return NULL;
	}
}
