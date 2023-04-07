# Nickel

Nickel is a tiny LISP-like language. I have used it to help me teach
programming language concepts such as various aspects of functional
programming as well as methods used to implement a programming language.
The Nickel interpreter serves as a simple example of a recursive-descent
parser, tree-walking interpreter, and dynamic type checker.

## Requirements
- A POSIX environment.
- GCC supporting the C99 standard. (`clang` surely work, but the build script uses `gcc`.)

## Building
```bash
./build.sh```

## Running
```bash
./nickel examples/hello.nickel```
