// ----------------------------------------------------------------------- //
//
// MODULE  : butemgr_impl.cpp
//
// PURPOSE : CButeMgr -- NOLF 1's attribute-file reader.
//
// WHY THIS EXISTS
//
//   LT2/lithshared ships headers only; the original build linked a prebuilt
//   ButeMgrNoMFC.lib. Only CButeMgr::Parse has inline bodies in ButeMgr.h --
//   the scanner, the parser and every accessor are bare declarations, so they
//   are defined here, against the SDK's own declarations. The header is not
//   modified.
//
// THE GRAMMAR, MEASURED FROM THE SHIPPED FILES (Docs/FORMATS.md §5)
//
//     comment   := "//" ... eol   |   "/*" ... "*/"
//     tag       := '[' ident ']'
//     statement := ident '=' value
//     value     := int | float | "string"
//                | '(' num ',' num [',' num ',' num] ')'     point / rect
//                | '<' num ',' num ',' num '>'               vector
//                | '[' num ',' num ']'                       range
//
//   ⚠️ '[' is BOTH the tag opener at statement level AND the range opener after
//   '='. Position disambiguates -- Statement() only scans a value after it has
//   consumed '=', so the two never compete.
//
// CONTRACT WITH THE INLINE Parse()
//
//   Parse() sets m_pData to an istrstream over the buffer, calls Reset(),
//   clears the three tables, then calls TagList() and deletes m_pData. So
//   TagList() must drive everything from m_pData and report success/failure.
//
// ----------------------------------------------------------------------- //

// ⚠️ INCLUDE ORDER MATTERS and must mirror the game's own Stdafx.h:
//   windows.h   -- defines _WINDOWS_, so mfcstub defers to its POINT/RECT
//   lithtech.h  -- brings in LTVector, which avector.h's conversion operator
//                  needs, and defines _LITHTECH_ which guards that operator
//   mfcstub.h   -- CString / CPoint / CRect, used throughout ButeMgr.h
// Getting this wrong compiles this file against different types than every
// game translation unit sees.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>

#include "ButeMgr.h"

// ----------------------------------------------------------------------- //
// Token scheme.
//
// m_token / m_tokenMinor are private shorts used only by this file, so the
// encoding is ours to choose.
// ----------------------------------------------------------------------- //

enum
{
    TOK_EOF = 0,
    TOK_IDENT,          // bare word; text in m_szTokenString
    TOK_NUMBER,         // int or float; text in m_szTokenString
    TOK_STRING,         // quoted; unquoted text in m_szTokenString
    TOK_LBRACKET,       // [
    TOK_RBRACKET,       // ]
    TOK_LPAREN,         // (
    TOK_RPAREN,         // )
    TOK_LANGLE,         // <
    TOK_RANGLE,         // >
    TOK_COMMA,          // ,
    TOK_EQUALS,         // =
    TOK_ERROR
};

// m_tokenMinor distinguishes integer from real for TOK_NUMBER.
enum { NUM_INT = 0, NUM_REAL = 1 };

// Character classes, as reported by CharClass().
enum
{
    CC_EOF = 0, CC_SPACE, CC_ALPHA, CC_DIGIT, CC_QUOTE,
    CC_SLASH, CC_PUNCT, CC_OTHER
};

// ----------------------------------------------------------------------- //
// Scanner
// ----------------------------------------------------------------------- //

short CButeMgr::CharClass(unsigned char c)
{
    if (c == 0)                              return CC_EOF;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return CC_SPACE;
    if (isalpha(c) || c == '_')              return CC_ALPHA;
    if (isdigit(c))                          return CC_DIGIT;
    if (c == '"')                            return CC_QUOTE;
    if (c == '/')                            return CC_SLASH;
    if (strchr("[]()<>,=+-.", (char)c))      return CC_PUNCT;
    return CC_OTHER;
}

// The scanner is direct rather than table-driven, so these two exist to honour
// the declared interface and to keep the classification in one place.
short CButeMgr::NextState(short State, unsigned char c)
{
    return (short)((State * 8) + CharClass(c));
}

short CButeMgr::Action(short State, unsigned char c)
{
    return CharClass(c);
}

void CButeMgr::LookupCodes(short State, unsigned char c)
{
    m_tokenMinor = CharClass(c);
    (void)State;
}

// Pull one character into m_currentChar. Honours the one-character pushback
// (m_bPutChar) and keeps m_lineNumber accurate for error messages.
void CButeMgr::ConsumeChar()
{
    if (m_bPutChar)
    {
        m_bPutChar = false;
        return;
    }

    if (!m_pData || !m_pData->good())
    {
        m_currentChar = 0;
        return;
    }

    int ch = m_pData->get();
    if (ch < 0)
    {
        m_currentChar = 0;
        return;
    }

    m_currentChar = (unsigned char)ch;

    if (m_currentChar == '\n' && m_bLineCounterFlag)
        ++m_lineNumber;
}

// Produce the next token into m_token / m_tokenMinor / m_szTokenString.
bool CButeMgr::ScanTok()
{
    m_szTokenString[0] = '\0';

    // Skip whitespace and both comment forms.
    for (;;)
    {
        ConsumeChar();

        if (m_currentChar == 0)
        {
            m_token = TOK_EOF;
            return true;
        }

        if (CharClass(m_currentChar) == CC_SPACE)
            continue;

        if (m_currentChar == '/')
        {
            ConsumeChar();
            if (m_currentChar == '/')
            {
                while (m_currentChar != 0 && m_currentChar != '\n')
                    ConsumeChar();
                continue;
            }
            if (m_currentChar == '*')
            {
                unsigned char prev = 0;
                for (;;)
                {
                    ConsumeChar();
                    if (m_currentChar == 0)
                        break;                      // unterminated: treat as EOF
                    if (prev == '*' && m_currentChar == '/')
                        break;
                    prev = m_currentChar;
                }
                continue;
            }
            // A lone '/' is not part of this grammar.
            m_token = TOK_ERROR;
            return false;
        }

        break;
    }

    // Single-character punctuation.
    switch (m_currentChar)
    {
        case '[': m_token = TOK_LBRACKET; return true;
        case ']': m_token = TOK_RBRACKET; return true;
        case '(': m_token = TOK_LPAREN;   return true;
        case ')': m_token = TOK_RPAREN;   return true;
        case '<': m_token = TOK_LANGLE;   return true;
        case '>': m_token = TOK_RANGLE;   return true;
        case ',': m_token = TOK_COMMA;    return true;
        case '=': m_token = TOK_EQUALS;   return true;
        default: break;
    }

    // Quoted string. The stored text excludes the quotes.
    if (m_currentChar == '"')
    {
        int n = 0;
        for (;;)
        {
            ConsumeChar();
            if (m_currentChar == 0 || m_currentChar == '"')
                break;
            if (n < (int)sizeof(m_szTokenString) - 1)
                m_szTokenString[n++] = (char)m_currentChar;
        }
        m_szTokenString[n] = '\0';
        m_token = TOK_STRING;
        return true;
    }

    // Number: optional sign, digits, optional fraction/exponent.
    if (isdigit(m_currentChar) || m_currentChar == '-' || m_currentChar == '+' ||
        m_currentChar == '.')
    {
        int n = 0;
        bool bReal = (m_currentChar == '.');

        m_szTokenString[n++] = (char)m_currentChar;
        for (;;)
        {
            ConsumeChar();
            if (m_currentChar == 0)
                break;
            if (isdigit(m_currentChar))
            {
                // keep going
            }
            else if (m_currentChar == '.' || m_currentChar == 'e' || m_currentChar == 'E')
            {
                bReal = true;
            }
            else if ((m_currentChar == '-' || m_currentChar == '+') &&
                     n > 0 && (m_szTokenString[n-1] == 'e' || m_szTokenString[n-1] == 'E'))
            {
                // exponent sign
            }
            else if (m_currentChar == 'f' || m_currentChar == 'F')
            {
                // C-style float suffix, e.g. 10.0f. Consume it and stop; it is
                // a type marker, not part of the numeric text.
                bReal = true;
                break;
            }
            else
            {
                m_bPutChar = true;              // not ours: give it back
                break;
            }

            if (n < (int)sizeof(m_szTokenString) - 1)
                m_szTokenString[n++] = (char)m_currentChar;
        }
        m_szTokenString[n] = '\0';
        m_token      = TOK_NUMBER;
        m_tokenMinor = bReal ? NUM_REAL : NUM_INT;
        return true;
    }

    // Identifier.
    if (CharClass(m_currentChar) == CC_ALPHA)
    {
        int n = 0;
        m_szTokenString[n++] = (char)m_currentChar;
        for (;;)
        {
            ConsumeChar();
            if (m_currentChar == 0)
                break;
            if (isalnum(m_currentChar) || m_currentChar == '_')
            {
                if (n < (int)sizeof(m_szTokenString) - 1)
                    m_szTokenString[n++] = (char)m_currentChar;
            }
            else
            {
                m_bPutChar = true;
                break;
            }
        }
        m_szTokenString[n] = '\0';
        m_token = TOK_IDENT;
        return true;
    }

    m_token = TOK_ERROR;
    return false;
}

bool CButeMgr::Match(int tok)
{
    if (m_token != tok)
        return false;
    return ScanTok();
}

// ----------------------------------------------------------------------- //
// Parser
// ----------------------------------------------------------------------- //

// Reads a numeric literal, returning it as a double. Assumes the current token
// is TOK_NUMBER.
static bool ltbute_Num(CButeMgr* pMgr, const char* szText, double& out)
{
    (void)pMgr;
    out = atof(szText);
    return true;
}

// value := number | string | '(' .. ')' | '<' .. '>' | '[' .. ']'
// Stores the parsed value into pTab under szKey.
bool CButeMgr::Statement()
{
    if (m_token != TOK_IDENT)
        return false;

    CString sKey = m_szTokenString;
    if (!Match(TOK_IDENT))
    {
        DisplayMessage("Line %d: bad key name", m_lineNumber);
        return false;
    }

    if (!Match(TOK_EQUALS))
    {
        DisplayMessage("Line %d: expected '=' after '%s'", m_lineNumber, (LPCTSTR)sKey);
        return false;
    }

    if (!m_pCurrTabOfItems)
        return false;

    CSymTabItem* pItem = 0;

    switch (m_token)
    {
        case TOK_STRING:
        {
            CString sVal = m_szTokenString;
            pItem = new CSymTabItem(StringType, sVal);
            if (!Match(TOK_STRING)) return false;
            break;
        }

        case TOK_NUMBER:
        {
            bool bReal = (m_tokenMinor == NUM_REAL);
            double d = atof(m_szTokenString);
            if (bReal) pItem = new CSymTabItem(DoubleType, d);
            else       pItem = new CSymTabItem(IntType, (int)d);
            if (!Match(TOK_NUMBER)) return false;
            break;
        }

        // '(' a ',' b [',' c ',' d] ')'  -- CPoint (2) or CRect (4)
        case TOK_LPAREN:
        {
            if (!Match(TOK_LPAREN)) return false;

            // ⚠️ '(' is ambiguous here. A NUMBER after it means a point/rect;
            // an IDENT means a C-style cast, e.g. (DWORD)64. Decide on the
            // token, not on the bracket.
            if (m_token == TOK_IDENT)
            {
                char szType[64];
                strncpy(szType, m_szTokenString, sizeof(szType) - 1);
                szType[sizeof(szType) - 1] = '\0';

                if (!Match(TOK_IDENT))  return false;
                if (!Match(TOK_RPAREN)) return false;

                if (m_token != TOK_NUMBER)
                {
                    DisplayMessage("Line %d: expected a number after (%s) for '%s'",
                                   m_lineNumber, szType, (LPCTSTR)sKey);
                    return false;
                }

                double dv = atof(m_szTokenString);
                if (!Match(TOK_NUMBER)) return false;

                if      (!strcasecmp(szType, "DWORD"))  pItem = new CSymTabItem(DwordType,  (DWORD)dv);
                else if (!strcasecmp(szType, "BYTE"))   pItem = new CSymTabItem(ByteType,   (BYTE)dv);
                else if (!strcasecmp(szType, "bool"))   pItem = new CSymTabItem(BoolType,   dv != 0.0);
                else if (!strcasecmp(szType, "float"))  pItem = new CSymTabItem(FloatType,  (float)dv);
                else if (!strcasecmp(szType, "double")) pItem = new CSymTabItem(DoubleType, dv);
                else                                    pItem = new CSymTabItem(IntType,    (int)dv);
                break;
            }

            double v[4] = {0,0,0,0};
            int n = 0;
            for (;;)
            {
                if (m_token != TOK_NUMBER) { DisplayMessage("Line %d: expected a number in (...) for '%s'", m_lineNumber, (LPCTSTR)sKey); return false; }
                if (n < 4) ltbute_Num(this, m_szTokenString, v[n]);
                ++n;
                if (!Match(TOK_NUMBER)) return false;
                if (m_token == TOK_COMMA) { if (!Match(TOK_COMMA)) return false; continue; }
                break;
            }
            if (!Match(TOK_RPAREN)) return false;

            if (n >= 4)
            {
                CRect r((SDWORD)v[0], (SDWORD)v[1], (SDWORD)v[2], (SDWORD)v[3]);
                pItem = new CSymTabItem(RectType, r);
            }
            else
            {
                CPoint p((SDWORD)v[0], (SDWORD)v[1]);
                pItem = new CSymTabItem(PointType, p);
            }
            break;
        }

        // '<' x ',' y ',' z '>'  -- CAVector
        case TOK_LANGLE:
        {
            if (!Match(TOK_LANGLE)) return false;
            double v[3] = {0,0,0};
            for (int i = 0; i < 3; ++i)
            {
                if (m_token != TOK_NUMBER) return false;
                ltbute_Num(this, m_szTokenString, v[i]);
                if (!Match(TOK_NUMBER)) return false;
                if (i < 2 && !Match(TOK_COMMA)) return false;
            }
            if (!Match(TOK_RANGLE)) return false;
            CAVector vec(v[0], v[1], v[2]);
            pItem = new CSymTabItem(VectorType, vec);
            break;
        }

        // '[' lo ',' hi ']'  -- CARange. Only reachable AFTER '=', which is what
        // keeps it from colliding with the tag opener.
        case TOK_LBRACKET:
        {
            if (!Match(TOK_LBRACKET)) return false;
            double lo = 0, hi = 0;
            if (m_token != TOK_NUMBER) return false;
            ltbute_Num(this, m_szTokenString, lo);
            if (!Match(TOK_NUMBER)) return false;
            if (!Match(TOK_COMMA))  return false;
            if (m_token != TOK_NUMBER) return false;
            ltbute_Num(this, m_szTokenString, hi);
            if (!Match(TOK_NUMBER)) return false;
            if (!Match(TOK_RBRACKET)) return false;
            CARange rg(lo, hi);
            pItem = new CSymTabItem(RangeType, rg);
            break;
        }

        // A bare word as a value: stored as a string, which is what the
        // original data's occasional unquoted values need.
        //
        // ⚠️ EXCEPT TRUE AND FALSE, WHICH ARE RESERVED WORDS (HANDOVER 5cf):
        // the donor's lexer turns them into BoolTok whatever their case
        // (libs/ButeMgr/butemgr.cpp, CReservedWords) and stores a BoolType.
        // Stored as text, GetBool found no number in "TRUE" and answered
        // false -- every one of AIButes.txt's ~1,030 sense switches
        // (CanSeeEnemy, CanHearEnemyWeaponImpact, ...), so NO AI in the game
        // could see or hear the player, and T01S02's sleeping researcher
        // (IHEWeaponImpact1st = "IDLE POSE=SIT") never woke at the window.
        // LT_OLDBUTEBOOL=1 keeps them as text (says so).
        case TOK_IDENT:
        {
            static const bool s_bOld = []() {
                const bool b = (getenv("LT_OLDBUTEBOOL") != 0);
                if (b) { printf("[BUTE] LT_OLDBUTEBOOL=1 -- TRUE/FALSE read as text, every GetBool false (old)\n"); fflush(stdout); }
                return b; }();
            const bool bTrue  = !s_bOld && !strcasecmp(m_szTokenString, "TRUE");
            const bool bFalse = !s_bOld && !strcasecmp(m_szTokenString, "FALSE");
            if (bTrue || bFalse)
                pItem = new CSymTabItem(BoolType, bTrue);
            else
            {
                CString sVal = m_szTokenString;
                pItem = new CSymTabItem(StringType, sVal);
            }
            if (!Match(TOK_IDENT)) return false;
            break;
        }

        default:
            DisplayMessage("Line %d: unexpected value token %d for key '%s' (text \"%s\")",
                           m_lineNumber, (int)m_token, (LPCTSTR)sKey, m_szTokenString);
            return false;
    }

    if (!pItem)
        return false;

    m_pCurrTabOfItems->add((LPCTSTR)sKey, pItem);
    return true;
}

bool CButeMgr::StatementList()
{
    // Statements run until the next tag or end of input.
    while (m_token == TOK_IDENT)
    {
        if (!Statement())
            return false;
    }
    return true;
}

bool CButeMgr::Tag()
{
    if (!Match(TOK_LBRACKET))
        return false;

    if (m_token != TOK_IDENT && m_token != TOK_STRING)
    {
        DisplayMessage("Line %d: expected a tag name", m_lineNumber);
        return false;
    }

    m_sTagName = m_szTokenString;
    if (!ScanTok())
        return false;

    if (!Match(TOK_RBRACKET))
    {
        DisplayMessage("Line %d: expected ']' after tag '%s'",
                       m_lineNumber, (LPCTSTR)m_sTagName);
        return false;
    }

    // Re-entering an existing tag keeps adding to it rather than replacing it.
    TableOfItems* pTab = m_tagTab.lookup((LPCTSTR)m_sTagName);
    if (!pTab)
    {
        pTab = new TableOfItems();
        m_tagTab.add((LPCTSTR)m_sTagName, pTab);
    }
    m_pCurrTabOfItems = pTab;

    return StatementList();
}

// The top-level production. Parse() calls exactly this.
bool CButeMgr::TagList()
{
    m_bLineCounterFlag = true;
    m_lineNumber       = 1;
    m_bPutChar         = false;

    if (!ScanTok())
    {
        DisplayMessage("Line %d: bad token at start of file", m_lineNumber);
        m_bSuccess = false;
        return false;
    }

    while (m_token != TOK_EOF)
    {
        if (m_token != TOK_LBRACKET)
        {
            DisplayMessage("Line %d: expected a '[tag]'", m_lineNumber);
            m_bSuccess = false;
            return false;
        }
        if (!Tag())
        {
            // Tag()/Statement() have already reported the specific reason.
            m_bSuccess = false;
            return false;
        }
    }

    m_bSuccess = true;
    return true;
}

// ----------------------------------------------------------------------- //
// Lifecycle
// ----------------------------------------------------------------------- //

CButeMgr::CButeMgr()
    : m_tagTab(), m_auxTagTab(), m_newTagTab()
{
    m_pData            = 0;
    m_pSaveData        = 0;
    m_pCurrTabOfItems  = 0;
    m_pDisplayFunc     = 0;
    m_decryptCode      = 0;
    m_checksum         = 0;
    m_lineNumber       = 1;
    m_bLineCounterFlag = true;
    m_bSuccess         = false;
    m_bErrorFlag       = false;
    m_currentChar      = 0;
    m_token            = TOK_EOF;
    m_tokenMinor       = 0;
    m_bPutChar         = false;
    m_bSaving          = false;
    m_bCrypt           = false;
    m_szTokenString[0] = '\0';
}

void CButeMgr::Init()
{
    Reset();
}

void CButeMgr::Init(void (*pF)(const char* szMsg))
{
    m_pDisplayFunc = pF;
    Reset();
}

// Resets the SCANNER and status only. Parse() clears the tables separately, so
// resetting them here would discard a table the caller just built.
void CButeMgr::Reset()
{
    m_lineNumber       = 1;
    m_bLineCounterFlag = true;
    m_currentChar      = 0;
    m_token            = TOK_EOF;
    m_tokenMinor       = 0;
    m_bPutChar         = false;
    m_bErrorFlag       = false;
    m_bSuccess         = false;
    m_checksum         = 0;
    m_szTokenString[0] = '\0';
    m_sErrorString     = "";
    m_pCurrTabOfItems  = 0;
}

void CButeMgr::DisplayMessage(const char* szMsg, ...)
{
    char szBuf[1024];
    va_list args;
    va_start(args, szMsg);
    vsnprintf(szBuf, sizeof(szBuf), szMsg, args);
    va_end(args);

    m_sErrorString = szBuf;

    if (m_pDisplayFunc)
        m_pDisplayFunc(szBuf);
}

// ----------------------------------------------------------------------- //
// Lookup helpers.
//
// CSymTabItem's members are public, so these can be file-static. The tables
// themselves are private, so the member functions do the table lookup and pass
// the item down.
// ----------------------------------------------------------------------- //

static bool ltbute_AsDouble(const CButeMgr::CSymTabItem* p, double& out)
{
    if (!p) return false;
    switch (p->SymType)
    {
        case CButeMgr::IntType:    out = (double)*p->data.i;    return true;
        case CButeMgr::DwordType:  out = (double)*p->data.dw;   return true;
        case CButeMgr::ByteType:   out = (double)*p->data.byte; return true;
        case CButeMgr::BoolType:   out = *p->data.b ? 1.0 : 0.0; return true;
        case CButeMgr::FloatType:  out = (double)*p->data.f;    return true;
        case CButeMgr::DoubleType: out = *p->data.d;            return true;
        default: return false;
    }
}

// Fallbacks for the reference-returning getters when a key is absent. The
// signatures return references, so a local temporary would dangle.
static CString  s_defString;
static CRect    s_defRect;
static CPoint   s_defPoint;
static CAVector s_defVector;
static CARange  s_defRange;

#define LTBUTE_FIND(tag, att)                                   \
    TableOfItems* pTab = m_tagTab.lookup(tag);                  \
    CSymTabItem*  pItem = pTab ? pTab->lookup(att) : 0

// ----------------------------------------------------------------------- //
// Scalar accessors
// ----------------------------------------------------------------------- //

int CButeMgr::GetInt(const char* t, const char* a, int defVal)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) return defVal;
    return (int)d;
}

int CButeMgr::GetInt(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d))
    {
        m_bErrorFlag = true;
        DisplayMessage("Missing int attribute [%s] %s", t ? t : "?", a ? a : "?");
        return 0;
    }
    return (int)d;
}

DWORD CButeMgr::GetDword(const char* t, const char* a, DWORD defVal)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) return defVal;
    return (DWORD)d;
}

DWORD CButeMgr::GetDword(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) { m_bErrorFlag = true; return 0; }
    return (DWORD)d;
}

BYTE CButeMgr::GetByte(const char* t, const char* a, BYTE defVal)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) return defVal;
    return (BYTE)d;
}

BYTE CButeMgr::GetByte(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) { m_bErrorFlag = true; return 0; }
    return (BYTE)d;
}

bool CButeMgr::GetBool(const char* t, const char* a, bool defVal)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) return defVal;
    return d != 0.0;
}

bool CButeMgr::GetBool(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) { m_bErrorFlag = true; return false; }
    return d != 0.0;
}

float CButeMgr::GetFloat(const char* t, const char* a, float defVal)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) return defVal;
    return (float)d;
}

float CButeMgr::GetFloat(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) { m_bErrorFlag = true; return 0.0f; }
    return (float)d;
}

double CButeMgr::GetDouble(const char* t, const char* a, double defVal)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) return defVal;
    return d;
}

double CButeMgr::GetDouble(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    double d;
    if (!ltbute_AsDouble(pItem, d)) { m_bErrorFlag = true; return 0.0; }
    return d;
}

// ----------------------------------------------------------------------- //
// String accessors
// ----------------------------------------------------------------------- //

CString& CButeMgr::GetString(const char* t, const char* a, CString& defVal)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != StringType) return defVal;
    return *pItem->data.s;
}

CString& CButeMgr::GetString(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != StringType)
    {
        m_bErrorFlag = true;
        s_defString = "";
        return s_defString;
    }
    return *pItem->data.s;
}

void CButeMgr::GetString(const char* t, const char* a, const char* defVal,
                         char* szResult, DWORD maxLen)
{
    if (!szResult || maxLen == 0) return;
    LTBUTE_FIND(t, a);
    const char* src = (pItem && pItem->SymType == StringType)
                    ? (LPCTSTR)*pItem->data.s
                    : (defVal ? defVal : "");
    strncpy(szResult, src ? src : "", maxLen - 1);
    szResult[maxLen - 1] = '\0';
}

void CButeMgr::GetString(const char* t, const char* a, char* szResult, DWORD maxLen)
{
    if (!szResult || maxLen == 0) return;
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != StringType)
    {
        m_bErrorFlag = true;
        szResult[0] = '\0';
        return;
    }
    const char* src = (LPCTSTR)*pItem->data.s;
    strncpy(szResult, src ? src : "", maxLen - 1);
    szResult[maxLen - 1] = '\0';
}

// ----------------------------------------------------------------------- //
// Compound accessors.
//
// ⚠️ These return REFERENCES, so they must never return a local. When the key
// is present the reference is into the stored item; otherwise it is the
// caller's own defVal, or a file-static fallback.
// ----------------------------------------------------------------------- //

CRect& CButeMgr::GetRect(const char* t, const char* a, CRect& defVal)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != RectType) return defVal;
    return *pItem->data.r;
}

CRect& CButeMgr::GetRect(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != RectType)
    {
        m_bErrorFlag = true;
        s_defRect = CRect(0,0,0,0);
        return s_defRect;
    }
    return *pItem->data.r;
}

CPoint& CButeMgr::GetPoint(const char* t, const char* a, CPoint& defVal)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != PointType) return defVal;
    return *pItem->data.point;
}

CPoint& CButeMgr::GetPoint(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != PointType)
    {
        m_bErrorFlag = true;
        s_defPoint = CPoint(0,0);
        return s_defPoint;
    }
    return *pItem->data.point;
}

CAVector& CButeMgr::GetVector(const char* t, const char* a, CAVector& defVal)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != VectorType) return defVal;
    return *pItem->data.v;
}

CAVector& CButeMgr::GetVector(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != VectorType)
    {
        m_bErrorFlag = true;
        s_defVector = CAVector(0,0,0);
        return s_defVector;
    }
    return *pItem->data.v;
}

CARange& CButeMgr::GetRange(const char* t, const char* a, CARange& defVal)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != RangeType) return defVal;
    return *pItem->data.range;
}

CARange& CButeMgr::GetRange(const char* t, const char* a)
{
    LTBUTE_FIND(t, a);
    if (!pItem || pItem->SymType != RangeType)
    {
        m_bErrorFlag = true;
        s_defRange = CARange(0,0);
        return s_defRange;
    }
    return *pItem->data.range;
}

// ----------------------------------------------------------------------- //
// Setters
// ----------------------------------------------------------------------- //

#define LTBUTE_SET(tag, type, value)                                        \
    do {                                                                    \
        TableOfItems* pTab = m_tagTab.lookup(tag);                          \
        if (!pTab) { pTab = new TableOfItems(); m_tagTab.add(tag, pTab); }   \
        pTab->add(a, new CSymTabItem(type, value));                          \
    } while (0)

void CButeMgr::SetInt(const char* t, const char* a, int v)                { LTBUTE_SET(t, IntType, v); }
void CButeMgr::SetDword(const char* t, const char* a, DWORD v)            { LTBUTE_SET(t, DwordType, v); }
void CButeMgr::SetByte(const char* t, const char* a, BYTE v)              { LTBUTE_SET(t, ByteType, v); }
void CButeMgr::SetBool(const char* t, const char* a, bool v)              { LTBUTE_SET(t, BoolType, v); }
void CButeMgr::SetFloat(const char* t, const char* a, float v)            { LTBUTE_SET(t, FloatType, v); }
void CButeMgr::SetDouble(const char* t, const char* a, double v)          { LTBUTE_SET(t, DoubleType, v); }
void CButeMgr::SetString(const char* t, const char* a, const CString& v)  { LTBUTE_SET(t, StringType, v); }
void CButeMgr::SetRect(const char* t, const char* a, const CRect& v)      { LTBUTE_SET(t, RectType, v); }
void CButeMgr::SetPoint(const char* t, const char* a, const CPoint& v)    { LTBUTE_SET(t, PointType, v); }
void CButeMgr::SetVector(const char* t, const char* a, const CAVector& v) { LTBUTE_SET(t, VectorType, v); }
void CButeMgr::SetRange(const char* t, const char* a, const CARange& v)   { LTBUTE_SET(t, RangeType, v); }

// ----------------------------------------------------------------------- //
// Tags
// ----------------------------------------------------------------------- //

bool CButeMgr::AddTag(const char* szTagName)
{
    if (!szTagName || !*szTagName)
        return false;

    if (m_tagTab.lookup(szTagName))
        return false;               // already present

    m_tagTab.add(szTagName, new TableOfItems());
    return true;
}

// szAttName == NULL asks only whether the TAG exists.
bool CButeMgr::Exist(const char* szTagName, const char* szAttName)
{
    if (!szTagName)
        return false;

    TableOfItems* pTab = m_tagTab.lookup(szTagName);
    if (!pTab)
        return false;
    if (!szAttName)
        return true;

    return pTab->lookup(szAttName) != 0;
}

// ----------------------------------------------------------------------- //
// Traversal
// ----------------------------------------------------------------------- //

void CButeMgr::TagTraverseFunc(const char* szTag, TableOfItems* pData, void* ExtraData)
{
    (void)pData;
    TagExtraData* pEx = (TagExtraData*)ExtraData;
    if (pEx && pEx->m_pCallback)
        pEx->m_pCallback(szTag, pEx->m_pAux);
}

void CButeMgr::KeyTraverseFunc(const char* szKey, CSymTabItem* pData, void* ExtraData)
{
    KeyExtraData* pEx = (KeyExtraData*)ExtraData;
    if (pEx && pEx->m_pCallback)
        pEx->m_pCallback(szKey, pData, pEx->m_pAux);
}

void CButeMgr::GetTags(void (*pCallback)(const char* szTag, void* pAux), void* pAux)
{
    if (!pCallback)
        return;
    TagExtraData extra(pCallback, pAux);
    m_tagTab.traverse(TagTraverseFunc, &extra);
}

void CButeMgr::GetKeys(const char* szTag,
                       void (*pCallback)(const char* szKey, CSymTabItem* pData, void* pAux),
                       void* pAux)
{
    if (!szTag || !pCallback)
        return;
    TableOfItems* pTab = m_tagTab.lookup(szTag);
    if (!pTab)
        return;
    KeyExtraData extra(pCallback, pAux);
    pTab->traverse(KeyTraverseFunc, &extra);
}

// ----------------------------------------------------------------------- //
// Save
// ----------------------------------------------------------------------- //

namespace
{
    // NB: no TableOfItems* here -- that typedef is PRIVATE to CButeMgr and
    // cannot be named at file scope. The traversal passes it as a parameter.
    struct LTButeSaveCtx
    {
        FILE* fp;
    };
    LTButeSaveCtx g_ltSaveCtx = { 0 };
}

static void ltbute_WriteItem(const char* szKey, CButeMgr::CSymTabItem* pItem, void* /*pAux*/)
{
    FILE* fp = g_ltSaveCtx.fp;
    if (!fp || !pItem || !szKey)
        return;

    switch (pItem->SymType)
    {
        case CButeMgr::IntType:    fprintf(fp, "\t%s = %d\n",  szKey, *pItem->data.i); break;
        case CButeMgr::DwordType:  fprintf(fp, "\t%s = %u\n",  szKey, *pItem->data.dw); break;
        case CButeMgr::ByteType:   fprintf(fp, "\t%s = %u\n",  szKey, (unsigned)*pItem->data.byte); break;
        case CButeMgr::BoolType:   fprintf(fp, "\t%s = %d\n",  szKey, *pItem->data.b ? 1 : 0); break;
        case CButeMgr::FloatType:  fprintf(fp, "\t%s = %f\n",  szKey, (double)*pItem->data.f); break;
        case CButeMgr::DoubleType: fprintf(fp, "\t%s = %f\n",  szKey, *pItem->data.d); break;
        case CButeMgr::StringType: fprintf(fp, "\t%s = \"%s\"\n", szKey, (LPCTSTR)*pItem->data.s); break;
        case CButeMgr::RectType:
            fprintf(fp, "\t%s = (%d, %d, %d, %d)\n", szKey,
                    (int)pItem->data.r->left,  (int)pItem->data.r->top,
                    (int)pItem->data.r->right, (int)pItem->data.r->bottom);
            break;
        case CButeMgr::PointType:
            fprintf(fp, "\t%s = (%d, %d)\n", szKey,
                    (int)pItem->data.point->x, (int)pItem->data.point->y);
            break;
        case CButeMgr::VectorType:
            fprintf(fp, "\t%s = <%f, %f, %f>\n", szKey,
                    pItem->data.v->Geti(), pItem->data.v->Getj(), pItem->data.v->Getk());
            break;
        case CButeMgr::RangeType:
            fprintf(fp, "\t%s = [%f, %f]\n", szKey,
                    pItem->data.range->GetMin(), pItem->data.range->GetMax());
            break;
        default: break;
    }
}

void CButeMgr::AuxTabItemsSave(const char* sz, CSymTabItem* pItem, void* p)
{
    ltbute_WriteItem(sz, pItem, p);
}

void CButeMgr::NewTabsSave(const char* sz, TableOfItems* pTabOfItems, void* p)
{
    (void)p;
    if (!g_ltSaveCtx.fp || !sz || !pTabOfItems)
        return;
    fprintf(g_ltSaveCtx.fp, "\n[%s]\n", sz);
    pTabOfItems->traverse(AuxTabItemsSave, 0);
}

// Writes the table back out in the same grammar it was read from.
bool CButeMgr::Save(const char* szNewFileName)
{
    const char* szFile = szNewFileName ? szNewFileName : (LPCTSTR)m_sAttributeFilename;
    if (!szFile || !*szFile)
    {
        DisplayMessage("CButeMgr::Save -- no filename");
        return false;
    }

    FILE* fp = fopen(szFile, "w");
    if (!fp)
    {
        DisplayMessage("CButeMgr::Save -- could not open '%s'", szFile);
        return false;
    }

    m_bSaving        = true;
    g_ltSaveCtx.fp = fp;

    m_tagTab.traverse(NewTabsSave, 0);

    g_ltSaveCtx.fp = 0;
    m_bSaving      = false;
    fclose(fp);
    return true;
}
