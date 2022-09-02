# fts5_expose

This is a small extension to expose internal FTS5 components of SQLite.

## fts5_tokenize()

`fts5_tokenize` becomes available after loading the library and exposes the internal tokenizer api.
All FTS5 tokenizers are supported. Default is unicode. Tokenizer can be configured via second parameter, 
equivalent to `tokenizer=` in the fts5 vtab creation.

For more information see https://www.sqlite.org/fts5.html#tokenizers

Return value is a JSON array of processed tokens. Make sure you compile SQLite with `json1` support.

Example:
```bash
sqlite> .load 'fts5_expose.so'
sqlite> select fts5_tokenize('hello please tokenize meee');
["hello","please","tokenize","meee"]
Run Time: real 0.001 user 0.000000 sys 0.000284
sqlite> select fts5_tokenize('unicode61', 'remove_diacritics 2', 'ö ü ä ß');
["o","u","a","ß"]
Run Time: real 0.000 user 0.000000 sys 0.000234
```