/* test_main.cpp: テスト用プログラムのメイン。
   Copyright (c) 2011 Ishibashi Hironori

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "misaki.h"

#include "misaki_debug.h"

using namespace Misaki;

// テスト用タイトルをプリントする。
void PrintTitle() {
  std::cout << "************" << std::endl;
  std::cout << "* Test Run *" << std::endl;
  std::cout << "************" << std::endl;
}

int main(int argc, char* argv[]) {
  // テスト用タイトルをプリント。
  PrintTitle();

  Init();

  ChessBoard* board = ChessBoard::New();

  board->Test();

  delete board;

  return 0;
}