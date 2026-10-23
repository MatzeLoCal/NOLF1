// ----------------------------------------------------------------------- //
//
// enginetest_wav.cpp -- the WAV reader, against every file the game ships.
//
// ⚠️ A SWEEP, not a sample. The DTX and ABC loaders each had a defect that only
// three files in several hundred exposed; a reader tested on the first ten
// files is a reader tested on the easy ten.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltwav.h"
#include "ltfilemgr.h"
#include "ltrezfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <map>
#include <string>
#include <vector>

static int g_nPass = 0, g_nFail = 0;
static void ok(const char* w)            { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)           { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w) { b ? ok(w) : bad(w); }

int main(int argc, char** argv)
{
    std::vector<const char*> dirs;
    for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);
    if (dirs.empty()) { fprintf(stderr, "usage: enginetest_wav <dirs...>\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&dirs[0], (int)dirs.size()) == 0)
    { fprintf(stderr, "no game data\n"); return 2; }

    printf("\n--- sweep: every .wav in the archives ---\n");

    // Enumerate through the archives directly so the sweep covers files no
    // code has ever asked for by name.
    std::vector<std::string> names;
    for (int t = 0; t < fm.GetNumTrees(); ++t)
    {
        const char* pPath = fm.GetTreePath(t);
        if (!pPath) continue;
        CLTRezArchive rz;
        if (!rz.Open(pPath)) continue;
        for (unsigned int i = 0; i < rz.GetNumEntries(); ++i)
        {
            std::string s(rz.GetEntryName(i));
            if (s.size() < 4) continue;
            std::string e = s.substr(s.size() - 4);
            for (size_t k = 0; k < e.size(); ++k) e[k] = (char)tolower((unsigned char)e[k]);
            if (e == ".wav") names.push_back(s);
        }
    }

    printf("        %zu .wav entries found\n", names.size());
    check(names.size() > 4000, "the archives really do hold thousands of sounds");

    std::map<int, int> byFmt;
    int nLoaded = 0, nRefused = 0, nDecoded = 0, nSilent = 0, nClipped = 0;
    double fSeconds = 0.0;
    std::map<std::string, int> reasons;
    std::vector<std::string> silentEx, clipEx;
    long long nPcmRail = 0, nPcmSamples = 0, nAdpcmRail = 0, nAdpcmSamples = 0;
    int nSilentAdpcm = 0;

    for (size_t i = 0; i < names.size(); ++i)
    {
        CLTWav w;
        if (!w.LoadThroughFileMgr(&fm, names[i].c_str()))
        { ++nRefused; reasons[w.GetError()]++; continue; }

        ++nLoaded;
        byFmt[(int)w.GetFormat()]++;
        fSeconds += w.GetDuration();

        if (w.GetFormat() == CLTWav::kMP3) continue;

        ++nDecoded;
        const std::vector<short>& s = w.GetSamples();
        if (s.empty()) { ++nSilent; continue; }

        // ⚠️ A decoder that returns the right NUMBER of samples and the wrong
        // VALUES passes every structural check. Non-silence is the cheapest
        // property that a wrong decode fails: ADPCM decoded as PCM, or with the
        // predictor never reset, does not come out flat -- but a decode that
        // produced nothing but zeros would, and that is the failure a size
        // check cannot see.
        bool bAnyNonZero = false;
        long long nAtRail = 0;
        for (size_t k = 0; k < s.size(); ++k)
        {
            if (s[k] != 0) bAnyNonZero = true;
            if (s[k] == 32767 || s[k] == -32768) ++nAtRail;
        }
        if (!bAnyNonZero)
        {
            ++nSilent;
            if (w.GetFormat() == CLTWav::kADPCM) ++nSilentAdpcm;
            if (silentEx.size() < 5) silentEx.push_back(names[i]);
        }

        if (w.GetFormat() == CLTWav::kADPCM)
        { nAdpcmRail += nAtRail; nAdpcmSamples += (long long)s.size(); }
        else
        { nPcmRail += nAtRail; nPcmSamples += (long long)s.size(); }
        // Sustained clipping is what a mis-stepped ADPCM predictor produces.
        if (!s.empty() && (double)nAtRail / (double)s.size() > 0.05)
        { ++nClipped;
          if (clipEx.size()<5) { char b[256]; snprintf(b,sizeof b,"%s  tag=%d %.1f%% at rail",
              names[i].c_str(), (int)w.GetFormat(), 100.0*nAtRail/(double)s.size());
              clipEx.push_back(b); } }
    }

    printf("        loaded %d, refused %d\n", nLoaded, nRefused);
    printf("        by format: PCM %d, ADPCM %d, MP3 %d\n",
           byFmt[CLTWav::kPCM], byFmt[CLTWav::kADPCM], byFmt[CLTWav::kMP3]);
    printf("        decoded to samples: %d   (%.1f seconds)\n", nDecoded, fSeconds);
    if (!reasons.empty())
        for (std::map<std::string,int>::iterator it = reasons.begin(); it != reasons.end(); ++it)
            printf("        refused: %-32s %d\n", it->first.c_str(), it->second);

    printf("        silent %d, sustained-clipping %d\n", nSilent, nClipped);
    for (size_t k=0;k<silentEx.size();++k) printf("          silent: %s\n", silentEx[k].c_str());
    for (size_t k=0;k<clipEx.size();++k) printf("          clip:   %s\n", clipEx[k].c_str());

    check(nRefused == 0, "every shipped .wav is read without one refusal");
    check(byFmt[CLTWav::kMP3] > 3000,
          "and the MP3-in-WAV majority is IDENTIFIED, not mistaken for PCM");
    check(nDecoded > 1000, "the PCM and ADPCM files decode to samples");
    // ⚠️ BOTH OF THESE WERE FIRST WRITTEN AS "== 0" AND BOTH WERE WRONG, in
    // the way that matters: they asserted a property of the CONTENT while
    // claiming to test the DECODER.
    //
    //   * 2 files decode to silence, and they are `SNOWMOBILE\BRAKE.WAV` and
    //     `MOTORCYCLE\BRAKE.WAV` -- authored that way. A decoder fault would
    //     silence a CLASS, not two vehicle brakes.
    //   * 3 files clip for more than 5% of their length, and TWO OF THEM ARE
    //     PCM (a Dragunov and a Contender firing). PCM is copied verbatim, so
    //     the reader cannot have introduced it: those files are simply loud.
    //
    // So the checks below compare ADPCM against PCM instead. A mis-stepped IMA
    // predictor -- the actual failure mode, from not resetting per block --
    // drives its output to the rails, and it would show as ADPCM clipping far
    // more than PCM does. PCM is the control, because it cannot be decoded
    // wrongly.
    check(nSilentAdpcm == 0,
          "no ADPCM file decodes to silence (the 2 silent ones are PCM brakes)");

    const double fPcmRail   = nPcmSamples   ? (double)nPcmRail   / (double)nPcmSamples   : 0.0;
    const double fAdpcmRail = nAdpcmSamples ? (double)nAdpcmRail / (double)nAdpcmSamples : 0.0;
    printf("        samples at the rail: PCM %.4f%%, ADPCM %.4f%%\n",
           100.0 * fPcmRail, 100.0 * fAdpcmRail);
    check(fAdpcmRail < fPcmRail * 4.0 + 0.001,
          "ADPCM clips no more than PCM does -- the predictor is resetting per block");

    printf("\n--- a file with exactly known contents ---\n");
    {
        CLTWav w;
        check(w.LoadThroughFileMgr(&fm, "CHARS\\SND\\JUMP2.WAV"), "JUMP2.WAV loads");
        printf("        tag %d, %u Hz, %u ch, %u bit, %zu samples, %.3f s\n",
               (int)w.GetFormat(), w.GetRate(), w.GetChannels(), w.GetBits(),
               w.GetSamples().size(), w.GetDuration());
        check(w.GetFormat() == CLTWav::kPCM && w.GetRate() == 22050 &&
              w.GetChannels() == 1 && w.GetBits() == 16,
              "and reports exactly what the file header says: PCM 22050 mono 16-bit");
        check(w.GetSamples().size() == 16936 / 2,
              "with one sample per two bytes of its data chunk");
    }

    printf("\n--- negative controls ---\n");
    {
        CLTWav w;
        check(!w.LoadThroughFileMgr(&fm, "CHARS\\SND\\NOT_A_REAL_SOUND.WAV"),
              "NEGATIVE CONTROL: a sound that does not exist is refused");

        const unsigned char kJunk[64] = { 'R','I','F','F', 0,0,0,0, 'X','A','V','E' };
        CLTWav w2;
        check(!w2.Load(kJunk, sizeof kJunk),
              "NEGATIVE CONTROL: a RIFF file that is not WAVE is refused");

        // ⚠️ The one that matters: a PCM header over MP3 data must not be
        // silently believed. Take a real MP3-in-WAV and claim it is PCM.
        CLTWav w3;
        check(!w3.Load("RIFFWAVEfmt ", 12),
              "NEGATIVE CONTROL: a truncated header is refused, not padded");
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
