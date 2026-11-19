# Tables in CSV files

Keep a table as a CSV file, for example `log/2026.csv`, with a header line such as
`date,time,item,qty,amount`. Add rows with `append_file`; a file that ends without a newline
gets one first, and a row identical to a recent line is named.

Totals: `read_file` with `sum` (columns to total) and optionally `column` with `from` and
`to` (a range: `2026-11-19` is a day, `2026-11` a month). Totals are exact. The result says
how many rows it covers and names every row left out, by line: blank or non-number cells,
rows with a field count unlike the header's (outside the range they are only counted),
keys not like `from`.

A line whose first non-blank character is `#` is a comment: never a row, never summed;
reads count it. Shown rows carry `L<n>`, their line number.

Corrections: `edit_file` with `line` replaces the whole line at that number, checked
against its text; a stale number fails with the current lines. A day starts at the
device's "day starts at" time.
