Block content inside a cell, which a pipe table cannot carry.

| What it holds | How it comes out |
| --- | --- |
| a bullet list | - first item<br>- nested item<br>- third item |
| a heading and a quotation | A heading in a cell<br>and a quotation under it |
| two lines of code | `first(line);`<br>`second(line);` |
| a rule and a bookmark | <a id="in_a_cell"></a>after the rule |
| a hyperlink | [a link in a cell](https://example.invalid/cell) and [one to the bookmark](#in_a_cell) |
| an ordered list continuing outside | 1. one<br>2. two |

3. three, which continues the count from inside the cell

The last paragraph.
