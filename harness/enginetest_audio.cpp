// ----------------------------------------------------------------------- //
//
// enginetest_audio.cpp -- the platform half: MP3 decode, and the device.
//
// ⚠️ SEPARATE FROM THE `mixer` SUITE BECAUSE IT LINKS AUDIOTOOLBOX. The mixer
// is pure C++ and must stay testable with no frameworks at all; this is the one
// sound suite that needs the system audio stack, exactly as `metal` is the one
// renderer suite that needs a GPU.
//
// ⚠️ It does NOT open the output device. A test suite that starts making noise
// on a build machine is a test suite people turn off.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltwav.h"
#include "ltaudio.h"
#include "ltmixer.h"
#include "ltfilemgr.h"
#include "ltrezfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (dirs.empty()) { fprintf(stderr, "usage: enginetest_audio <dirs...>\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&dirs[0], (int)dirs.size()) == 0)
    { fprintf(stderr, "no game data\n"); return 2; }

    printf("\n--- sweep: every MP3-in-WAV in the archives ---\n");

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

    int nMp3 = 0, nDecoded = 0, nSilent = 0;
    double fSeconds = 0.0;
    std::vector<std::string> fails;

    for (size_t i = 0; i < names.size(); ++i)
    {
        CLTWav w;
        if (!w.LoadThroughFileMgr(&fm, names[i].c_str())) continue;
        if (w.GetFormat() != CLTWav::kMP3) continue;
        ++nMp3;

        std::vector<short> pcm; unsigned int rate = 0, ch = 0;
        if (!LTAudio_DecodeMP3(&w.GetRaw()[0], (unsigned int)w.GetRaw().size(), pcm, rate, ch))
        { if (fails.size() < 5) fails.push_back(names[i]); continue; }

        ++nDecoded;
        if (rate && ch) fSeconds += (double)pcm.size() / ch / rate;

        bool bAny = false;
        for (size_t k = 0; k < pcm.size() && !bAny; ++k) if (pcm[k]) bAny = true;
        if (!bAny) ++nSilent;
    }

    printf("        %d MP3-in-WAV files, %d decoded, %.1f seconds of audio\n",
           nMp3, nDecoded, fSeconds);
    for (size_t i = 0; i < fails.size(); ++i) printf("        failed: %s\n", fails[i].c_str());

    check(nMp3 > 3000, "the archives really are mostly MP3");
    check(nDecoded == nMp3, "and every one of them decodes");
    check(nSilent == 0, "with not one decoding to silence");
    check(fSeconds > 3600.0, "which is hours of audio, not a handful of clips");

    printf("\n--- negative controls ---\n");
    {
        std::vector<short> pcm; unsigned int rate = 0, ch = 0;
        const unsigned char kJunk[64] = { 0 };
        check(!LTAudio_DecodeMP3(kJunk, sizeof kJunk, pcm, rate, ch),
              "NEGATIVE CONTROL: a buffer of zeros is refused, not decoded to silence");
        check(!LTAudio_DecodeMP3(0, 0, pcm, rate, ch),
              "NEGATIVE CONTROL: nothing at all is refused");

        // ⚠️ PCM data handed to the MP3 decoder must be REFUSED. If it were
        // accepted, the format tag check in CLTWav would be the only thing
        // standing between a PCM file and a garbage decode.
        CLTWav w;
        if (w.LoadThroughFileMgr(&fm, "CHARS\\SND\\JUMP2.WAV") &&
            w.GetFormat() == CLTWav::kPCM && !w.GetRaw().empty())
        {
            check(!LTAudio_DecodeMP3(&w.GetRaw()[0], (unsigned int)w.GetRaw().size(),
                                     pcm, rate, ch),
                  "NEGATIVE CONTROL: raw PCM is refused by the MP3 decoder");
        }
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
