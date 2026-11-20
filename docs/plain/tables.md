# Tables in CSV files

Keep a table as a CSV file, for example `log/2026.csv`, with a header line such as
`date,time,item,qty,amount`. Add rows with `append_file`; a file that ends without a newline
gets one first, and a row identical to a recent line is named.

Rows: `read_file` with `column`, `from` and optionally `to` (`2026-11-19` is a day, `2026-11`
a month) is a plain read of the selected rows: the same head, the header and the rows as
stored. Only a row whose key reads and is out of range is left out; a row that does not
parse is kept. It totals nothing: add the rows with `calc`. A `limit` under 2,048 B reads as
2,048 B; `next_offset` gives the next part. A `#` line is a comment, never a row.

Corrections: `edit_file` with `line` changes only that line: `old` is the whole line or a
part of it found there once; a stale number fails with the current lines.

The old engine: `files.read` with `column`, `from`, `to` or `tail`; `files.edit` with
`find`, `replace`, `line`.
