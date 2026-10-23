// ----------------------------------------------------------------------- //
//
// MODULE  : ltproptypes.cpp
//
// See ltproptypes.h for why the world file's own Filename/Skin are not the
// answer for a PropType object.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltproptypes.h"

#include <stdio.h>
#include <stdlib.h>
#include "ltfilemgr.h"
#include "ltfilestream.h"

#include <string.h>
#include <vector>

namespace
{
    // `Key <tabs/spaces> = "value"` -> value, or false if this line is not
    // that key. ⚠️ The shipped file separates key and `=` with TABS, and pads
    // inconsistently, so nothing here may assume a fixed column.
    // ⚠️ The SAME key-matching rule as QuotedValue, for the numeric fields --
    // `Chromakey = 1` has no quotes. Sharing the "only whitespace between the
    // key and '='" check is what stops `Chrome` matching `Chromakey`, which
    // are BOTH real keys in this file and differ by three characters.
    bool IntValue(const std::string& sLine, const char* pKey, int& nOut)
    {
        size_t i = sLine.find_first_not_of(" \t");
        if (i == std::string::npos) return false;

        const size_t nKey = strlen(pKey);
        if (sLine.compare(i, nKey, pKey) != 0) return false;
        i += nKey;

        size_t j = sLine.find_first_not_of(" \t", i);
        if (j == std::string::npos || sLine[j] != '=') return false;

        const size_t a = sLine.find_first_not_of(" \t", j + 1);
        if (a == std::string::npos) return false;
        if (sLine[a] != '-' && (sLine[a] < '0' || sLine[a] > '9')) return false;

        nOut = atoi(sLine.c_str() + a);
        return true;
    }

    bool RealValue(const std::string& sLine, const char* pKey, float& fOut)
    {
        size_t i = sLine.find_first_not_of(" \t");
        if (i == std::string::npos) return false;

        const size_t nKey = strlen(pKey);
        if (sLine.compare(i, nKey, pKey) != 0) return false;
        i += nKey;

        size_t j = sLine.find_first_not_of(" \t", i);
        if (j == std::string::npos || sLine[j] != '=') return false;

        const size_t a = sLine.find_first_not_of(" \t", j + 1);
        if (a == std::string::npos) return false;
        if (sLine[a] != '-' && sLine[a] != '.' && (sLine[a] < '0' || sLine[a] > '9')) return false;

        fOut = (float)atof(sLine.c_str() + a);
        return true;
    }

    // `Scale = <1.0, 1.0, 1.0>` -- the bute file's vector form.
    bool VectorValue(const std::string& sLine, const char* pKey, float* pOut3)
    {
        size_t i = sLine.find_first_not_of(" \t");
        if (i == std::string::npos) return false;
        const size_t nKey = strlen(pKey);
        if (sLine.compare(i, nKey, pKey) != 0) return false;
        i += nKey;
        size_t j = sLine.find_first_not_of(" \t", i);
        if (j == std::string::npos || sLine[j] != '=') return false;
        const size_t a = sLine.find('<', j);
        if (a == std::string::npos) return false;
        float v[3];
        if (sscanf(sLine.c_str() + a + 1, "%f , %f , %f", &v[0], &v[1], &v[2]) != 3) return false;
        for (int k = 0; k < 3; ++k) pOut3[k] = v[k];
        return true;
    }

    bool QuotedValue(const std::string& sLine, const char* pKey, std::string& sOut)
    {
        size_t i = sLine.find_first_not_of(" \t");
        if (i == std::string::npos) return false;

        const size_t nKey = strlen(pKey);
        if (sLine.compare(i, nKey, pKey) != 0) return false;
        i += nKey;

        // Only whitespace may sit between the key and '='; otherwise this is a
        // DIFFERENT key that merely starts with the same letters -- `Type` and
        // `TypeName` would collide without this.
        size_t j = sLine.find_first_not_of(" \t", i);
        if (j == std::string::npos || sLine[j] != '=') return false;

        const size_t a = sLine.find('"', j);
        if (a == std::string::npos) return false;
        const size_t b = sLine.find('"', a + 1);
        if (b == std::string::npos) return false;

        sOut = sLine.substr(a + 1, b - a - 1);
        return true;
    }
}


bool CLTPropTypes::Load(CLTFileMgr* pFileMgr)
{
    m_bLoaded = false;
    m_Types.clear();
    if (!pFileMgr) return false;

    ILTStream* pStream = pFileMgr->OpenFile("Attributes\\PropTypes.txt");
    if (!pStream) return false;

    const unsigned long nLen = pStream->GetLen();
    std::vector<char> buf(nLen + 1, 0);
    if (nLen) pStream->Read(&buf[0], nLen);
    pStream->Release();

    // ---- walk it a line at a time -------------------------------------- //
    //
    // ⚠️ A section is only committed when the NEXT section starts or the file
    // ends, because Type/Filename/Skin appear in any order within it and the
    // record is not complete until the section is.
    Entry cur;
    std::string sType;

    const char* p = &buf[0];
    const char* pEnd = p + nLen;
    std::string sLine;

    while (true)
    {
        const bool bEof = (p >= pEnd);
        if (!bEof)
        {
            const char* q = p;
            while (q < pEnd && *q != '\n' && *q != '\r') ++q;
            sLine.assign(p, (size_t)(q - p));
            p = q;
            while (p < pEnd && (*p == '\n' || *p == '\r')) ++p;
        }

        const size_t i = bEof ? std::string::npos : sLine.find_first_not_of(" \t");
        const bool bSection = (!bEof && i != std::string::npos && sLine[i] == '[');

        if (bSection || bEof)
        {
            // ⚠️ Both must be present. A section with a Type and no Filename
            // is not a drawable prop, and storing it would make Find() answer
            // "yes, draw nothing" for a type the world does place.
            if (!sType.empty() && !cur.m_sFilename.empty())
                m_Types[sType] = cur;

            cur = Entry();
            sType.clear();
            if (bEof) break;
            continue;
        }

        std::string v;
        int nInt = 0;
        float fReal = 1.0f;
        if      (QuotedValue(sLine, "Type",     v)) sType = v;
        else if (QuotedValue(sLine, "Filename", v)) cur.m_sFilename = v;
        else if (QuotedValue(sLine, "Skin",     v)) cur.m_sSkin = v;
        else if (IntValue(sLine, "Chromakey", nInt))  cur.m_bChromakey = (nInt != 0);
        // ⚠️ `Alpha` is a FLOAT here (0.999), so IntValue's digit test accepts
        // it and atof reads it. Using IntValue would truncate every 0.999 to 0
        // and mark all 301 entries translucent.
        else if (RealValue(sLine, "Alpha", fReal))    cur.m_fAlpha = fReal;
        // ⚠️ Read with the same key rule, so `Additive` cannot match
        // `AdditiveFX` and `Multiply` cannot match something longer later.
        else if (IntValue(sLine, "Additive", nInt))   cur.m_bAdditive = (nInt != 0);
        else if (IntValue(sLine, "Multiply", nInt))   cur.m_bMultiply = (nInt != 0);
        else if (IntValue(sLine, "MoveToFloor", nInt)) cur.m_bMoveToFloor = (nInt != 0);
        else if (VectorValue(sLine, "Scale", cur.m_Scale)) {}
    }

    m_bLoaded = true;
    return true;
}


const CLTPropTypes::Entry* CLTPropTypes::Find(const std::string& sType) const
{
    std::map<std::string, Entry>::const_iterator it = m_Types.find(sType);
    return (it == m_Types.end()) ? 0 : &it->second;
}
