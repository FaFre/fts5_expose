#include <assert.h>
#include <string.h>

#include "fts5.h"

#include "sqlite3ext.h" /* Do not use <sqlite3.h>! */
SQLITE_EXTENSION_INIT1

#define MAX_TOKENS 256

#pragma region
static void *sqlite3Fts5MallocZero(int *pRc, sqlite3_int64 nByte) {
  void *pRet = 0;
  if (*pRc == SQLITE_OK) {
    pRet = sqlite3_malloc64(nByte);
    if (pRet == 0) {
      if (nByte > 0)
        *pRc = SQLITE_NOMEM;
    } else {
      memset(pRet, 0, (size_t)nByte);
    }
  }
  return pRet;
}

static int fts5_iswhitespace(char x) { return (x == ' '); }

/*
** Argument pIn points to a character that is part of a nul-terminated
** string. Return a pointer to the first character following *pIn in
** the string that is not a white-space character.
*/
static const char *fts5ConfigSkipWhitespace(const char *pIn) {
  const char *p = pIn;
  if (p) {
    while (fts5_iswhitespace(*p)) {
      p++;
    }
  }
  return p;
}

/*
** Return true if character 't' may be part of an FTS5 bareword, or false
** otherwise. Characters that may be part of barewords:
**
**   * All non-ASCII characters,
**   * The 52 upper and lower case ASCII characters, and
**   * The 10 integer ASCII characters.
**   * The underscore character "_" (0x5F).
**   * The unicode "subsitute" character (0x1A).
*/
static int sqlite3Fts5IsBareword(char t) {
  unsigned char aBareword[128] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x00 .. 0x0F */
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, /* 0x10 .. 0x1F */
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x20 .. 0x2F */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, /* 0x30 .. 0x3F */
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x40 .. 0x4F */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, /* 0x50 .. 0x5F */
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x60 .. 0x6F */
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0  /* 0x70 .. 0x7F */
  };

  return (t & 0x80) || aBareword[(int)t];
}

/*
** Argument pIn points to a character that is part of a nul-terminated
** string. Return a pointer to the first character following *pIn in
** the string that is not a "bareword" character.
*/
static const char *fts5ConfigSkipBareword(const char *pIn) {
  const char *p = pIn;
  while (sqlite3Fts5IsBareword(*p))
    p++;
  if (p == pIn)
    p = 0;
  return p;
}

static int fts5_isdigit(char a) { return (a >= '0' && a <= '9'); }

static const char *fts5ConfigSkipLiteral(const char *pIn) {
  const char *p = pIn;
  switch (*p) {
  case 'n':
  case 'N':
    if (sqlite3_strnicmp("null", p, 4) == 0) {
      p = &p[4];
    } else {
      p = 0;
    }
    break;

  case 'x':
  case 'X':
    p++;
    if (*p == '\'') {
      p++;
      while ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') ||
             (*p >= '0' && *p <= '9')) {
        p++;
      }
      if (*p == '\'' && 0 == ((p - pIn) % 2)) {
        p++;
      } else {
        p = 0;
      }
    } else {
      p = 0;
    }
    break;

  case '\'':
    p++;
    while (p) {
      if (*p == '\'') {
        p++;
        if (*p != '\'')
          break;
      }
      p++;
      if (*p == 0)
        p = 0;
    }
    break;

  default:
    /* maybe a number */
    if (*p == '+' || *p == '-')
      p++;
    while (fts5_isdigit(*p))
      p++;

    /* At this point, if the literal was an integer, the parse is
    ** finished. Or, if it is a floating point value, it may continue
    ** with either a decimal point or an 'E' character. */
    if (*p == '.' && fts5_isdigit(p[1])) {
      p += 2;
      while (fts5_isdigit(*p))
        p++;
    }
    if (p == pIn)
      p = 0;

    break;
  }

  return p;
}

/*
** The first character of the string pointed to by argument z is guaranteed
** to be an open-quote character (see function fts5_isopenquote()).
**
** This function searches for the corresponding close-quote character within
** the string and, if found, dequotes the string in place and adds a new
** nul-terminator byte.
**
** If the close-quote is found, the value returned is the byte offset of
** the character immediately following it. Or, if the close-quote is not
** found, -1 is returned. If -1 is returned, the buffer is left in an
** undefined state.
*/
static int fts5Dequote(char *z) {
  char q;
  int iIn = 1;
  int iOut = 0;
  q = z[0];

  /* Set stack variable q to the close-quote character */
  assert(q == '[' || q == '\'' || q == '"' || q == '`');
  if (q == '[')
    q = ']';

  while (z[iIn]) {
    if (z[iIn] == q) {
      if (z[iIn + 1] != q) {
        /* Character iIn was the close quote. */
        iIn++;
        break;
      } else {
        /* Character iIn and iIn+1 form an escaped quote character. Skip
        ** the input cursor past both and copy a single quote character
        ** to the output buffer. */
        iIn += 2;
        z[iOut++] = q;
      }
    } else {
      z[iOut++] = z[iIn++];
    }
  }

  z[iOut] = '\0';
  return iIn;
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
static void sqlite3Fts5Dequote(char *z) {
  char quote; /* Quote character (if any ) */

  assert(0 == fts5_iswhitespace(z[0]));
  quote = z[0];
  if (quote == '[' || quote == '\'' || quote == '"' || quote == '`') {
    fts5Dequote(z);
  }
}
#pragma endregion

/*
** Return a pointer to the fts5_api pointer for database connection db.
** If an error occurs, return NULL and leave an error in the database
** handle (accessible using sqlite3_errcode()/errmsg()).
*/
fts5_api *fts5_api_from_db(sqlite3 *db) {
  fts5_api *pRet = 0;
  sqlite3_stmt *pStmt = 0;

  if (SQLITE_OK == sqlite3_prepare(db, "SELECT fts5(?1)", -1, &pStmt, 0)) {
    sqlite3_bind_pointer(pStmt, 1, (void *)&pRet, "fts5_api_ptr", NULL);
    sqlite3_step(pStmt);
  }

  sqlite3_finalize(pStmt);
  return pRet;
}

struct Token {
  const char *pToken;
  struct Token *pNext;
};

static struct Token *getLastToken(struct Token *anyToken) {
  struct Token *lastToken = anyToken;
  while (lastToken->pNext != NULL) {
    lastToken = lastToken->pNext;
  }

  return lastToken;
}

static void freeTokenList(struct Token **firstToken) {
  struct Token *currentToken = *firstToken;
  do {
    struct Token *nextToken = currentToken->pNext;

    sqlite3_free((void *)currentToken->pToken);
    sqlite3_free(currentToken);

    currentToken = nextToken;
  } while (currentToken != NULL);

  *firstToken = NULL;
}

struct Fts5ExposeConfig {
  fts5_tokenizer tokenizer;
  Fts5Tokenizer *pTokenizer;
};

static struct Token *createToken() {
  struct Token *token = sqlite3_malloc64(sizeof(struct Token));
  memset(token, 0, sizeof(struct Token));

  return token;
}

static const char *sqlite3_strdup(const char *src, int n) {
  char *dest = sqlite3_malloc64(n + 1);
  memcpy((void *)dest, src, n);
  dest[n] = '\0';

  return dest;
}

static int
fts5TokenCallback(void *pContext,     /* Pointer to HighlightContext object */
                  int tflags,         /* Mask of FTS5_TOKEN_* flags */
                  const char *pToken, /* Buffer containing token */
                  int nToken,         /* Size of token in bytes */
                  int iStartOff,      /* Start offset of token */
                  int iEndOff         /* End offset of token */
) {
  struct Token *firstToken = (struct Token *)pContext;

  const char *tokenStr = sqlite3_strdup(pToken, nToken);

  if (firstToken->pToken == NULL) {
    firstToken->pToken = tokenStr;
  } else {
    struct Token *nextToken = createToken();
    nextToken->pToken = tokenStr;
    nextToken->pNext = NULL;

    struct Token *lastToken = getLastToken(firstToken);

    lastToken->pNext = nextToken;
  }

  return SQLITE_OK;
}

static int createTokenizer(fts5_api *fts5ApiPtr, char **pzErr,
                           const char *tokenizerName, const char *tokenizeParam,
                           struct Fts5ExposeConfig *config) {

  int rc = SQLITE_OK;

  sqlite3_int64 nArg = 0;
  char **azArg = NULL;
  char *pDel = NULL;

  if (fts5ApiPtr == NULL) {
    *pzErr = sqlite3_mprintf("Could not find FTS5 API");
    rc = SQLITE_ERROR;
  }

  if (rc == SQLITE_OK && tokenizeParam != NULL) {
    const char *p = tokenizeParam;

    nArg = strlen(tokenizeParam) + 1;
    azArg = sqlite3Fts5MallocZero(&rc, sizeof(char *) * nArg);

    pDel = sqlite3Fts5MallocZero(&rc, nArg * 2);
    char *pSpace = pDel;

    if (azArg && pSpace) {
      for (nArg = 0; p && *p; nArg++) {
        const char *p2 = fts5ConfigSkipWhitespace(p);

        if (*p2 == '\'') {
          p = fts5ConfigSkipLiteral(p2);
        } else {
          p = fts5ConfigSkipBareword(p2);
        }

        if (p) {
          memcpy(pSpace, p2, p - p2);
          azArg[nArg] = pSpace;
          sqlite3Fts5Dequote(pSpace);
          pSpace += (p - p2) + 1;
          p = fts5ConfigSkipWhitespace(p);
        }
      }

      if (p == 0) {
        *pzErr = sqlite3_mprintf("parse error in tokenize directive");
        rc = SQLITE_ERROR;
      }
    }
  }

  if (rc == SQLITE_OK) {
    void *pUserdata = 0;
    rc = fts5ApiPtr->xFindTokenizer(fts5ApiPtr, tokenizerName, &pUserdata,
                                    &config->tokenizer);
    if (rc == SQLITE_OK) {
      rc = config->tokenizer.xCreate(NULL, (const char **)azArg, nArg,
                                     &config->pTokenizer);
      if (rc != SQLITE_OK) {
        *pzErr = sqlite3_mprintf("could not create tokenizer with params '%s'",
                                 tokenizeParam);
      }
    } else {
      *pzErr = sqlite3_mprintf("could not find tokenizer %s", tokenizerName);
    }
  }

  sqlite3_free(azArg);
  sqlite3_free(pDel);

  return rc;
}

static void tokenizeSqlFunc(sqlite3_context *context, int argc,
                            sqlite3_value **argv) {
  int rc = SQLITE_OK;
  char *zErr = 0;

  if (argc > 3 || argc < 1) {
    sqlite3_result_error(context, "Invalid amount of arguments", -1);
  }

  const char *tokenizerName =
      (argc > 1) ? (const char *)sqlite3_value_text(argv[0]) : "unicode61";

  const char *tokenizerParam =
      (argc > 2) ? (const char *)sqlite3_value_text(argv[1]) : NULL;

  sqlite3 *db = sqlite3_context_db_handle(context);

  struct Fts5ExposeConfig config;
  memset(&config, 0, sizeof(struct Fts5ExposeConfig));

  fts5_api *fts5ApiPtr = fts5_api_from_db(db);
  rc = createTokenizer(fts5ApiPtr, &zErr, tokenizerName, tokenizerParam,
                       &config);

  if (rc == SQLITE_OK) {
    const char *zIn = (const char *)sqlite3_value_text(argv[argc - 1]);
    int nIn = sqlite3_value_bytes(argv[argc - 1]);

    struct Token *firstToken = createToken();

    config.tokenizer.xTokenize(config.pTokenizer, firstToken, FTS5_TOKENIZE_AUX,
                               zIn, nIn, fts5TokenCallback);

    const char *tokens[MAX_TOKENS] = {NULL};
    int tokenIndex = 0;
    {
      struct Token *currentToken = firstToken;
      for (; tokenIndex < MAX_TOKENS; tokenIndex++) {
        if (currentToken->pToken != NULL) {
          tokens[tokenIndex] = currentToken->pToken;
        }

        currentToken = currentToken->pNext;
        if (currentToken == NULL) {
          break;
        }
      }
    }

    int tokenCount = tokenIndex + 1;

    const char *query = "SELECT json_array(%s);";

    int paramLen = tokenCount * 2;
    char params[paramLen];
    for (int i = 0; i < paramLen; i += 2) {
      params[i] = '?';
      params[i + 1] = ',';
    }

    // Remove last comma and terminate string
    params[paramLen - 1] = '\0';

    const char *sql = sqlite3_mprintf(query, params);
    sqlite3_result_text(context, sql, -1, SQLITE_TRANSIENT);

    sqlite3_stmt *pStmt = 0;
    rc = sqlite3_prepare_v2(db, sql, -1, &pStmt, NULL);

    for (int i = 0; i < tokenCount; i++) {
      if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(pStmt, i + 1, tokens[i], -1, SQLITE_STATIC);
      }
    }

    if (rc == SQLITE_OK) {
      int stepRc = sqlite3_step(pStmt);
      if (stepRc == SQLITE_ROW) {
        sqlite3_result_text(context,
                            (const char *)sqlite3_column_text(pStmt, 0), -1,
                            SQLITE_TRANSIENT);
      } else if (stepRc != SQLITE_DONE) {
        rc = stepRc;
      }
    }

    sqlite3_finalize(pStmt);
    sqlite3_free((void *)sql);

    freeTokenList(&firstToken);

    config.tokenizer.xDelete(config.pTokenizer);
  }

  if (rc != SQLITE_OK) {
    if (zErr != NULL) {
      sqlite3_result_error(context, zErr, -1);
      sqlite3_free(zErr);
    } else {
      sqlite3_result_error(context, "Fatal error", -1);
    }
  }
}

#ifdef _WIN32
__declspec(dllexport)
#endif
    int sqlite3_ftsexpose_init(sqlite3 *db, char **pzErrMsg,
                               const sqlite3_api_routines *pApi) {
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);

  rc = sqlite3_create_function_v2(db, "fts5_tokenize", -1,
                                  SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                  tokenizeSqlFunc, 0, 0, 0);

  return rc;
}
