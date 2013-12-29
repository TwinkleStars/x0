
### Incomplete Flow Rewrite TODO

- DONE: codegen: native function
- DONE: codegen: custom handler invokation
- sema: native handler/function signature check (as a pass between parse and codegen)
- flowtool: auto-add source string as 2nd arg to `assert` + `assert_fail` handlers, via old onParseComplete hook

- data type code generation (for passing data to native functions/handlers)
  - DONE: number
  - DONE: boolean
  - DONE: string
  - buffer
  - ipaddr
  - cidr

- expression operator implementation
  - DONE: `expr and expr`
  - DONE: `expr xor expr`
  - DONE: `expr or expr`
  - DONE: `bool == bool`
  - DONE: `bool != bool`
  - DONE: (num, num): `== != < > <= >= + - * / mod shl shr & | ^ **`
  - (string, string): `== != < > <= >= + in`
  - (ip, ip): `== !=`
  - (cidr, cidr): `== != in`
  - (ip, cidr): `in`
  - (string, regex): `=~`