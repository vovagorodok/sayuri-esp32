/* The MIT License (MIT)
 *
 * Copyright (c) 2013-2016 Hironori Ishibashi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * @file lisp_core.cpp
 * @author Hironori Ishibashi
 * @brief Lispインタープリタの実装。 
 */

#include "lisp_core.h"

#include <iostream>
#include <cstdlib>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <map>
#include <functional>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <set>
#include <cmath>
#include <random>
#include <chrono>
#include <regex>
#include <ctime>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <system_error>

/** Sayuri 名前空間。 */
namespace Sayuri {
  // ========== //
  // static定数 //
  // ========== //
  const LPointer LObject::dummy_ptr_;
  const std::string LObject::dummy_str_;
  const LArgNames LObject::dummy_arg_names_;
  const LPointerVec LObject::dummy_ptr_vec_;
  const LScopeChain LObject::dummy_scope_chain_;
  const LC_Function LObject::dummy_c_function_;

  // ウォーカー。
  void Walk(LObject& pair, const LFuncForWalk& func) {
    // ペアじゃなければ終了。
    if (!(pair.IsPair())) return;

    // パス用文字バッファ。
    std::string path_buf;
    path_buf.reserve(256);

    // 再帰用関数を作成。
    std::function<void(LObject&)> core;
    core = [&func, &path_buf, &core](LObject& pair) {
      // ペアじゃなければ終了。
      if (!(pair.IsPair())) return;

      // 準備。 ポインタを記憶。
      LObject* car_before = pair.car().get();
      LObject* cdr_before = pair.cdr().get();

      // 関数を実行。
      func(pair, path_buf);

      // 終わり時のポインタを記録。
      LObject* car_after = pair.car().get();
      LObject* cdr_after = pair.cdr().get();

      // ポインタが同じならそれぞれに再帰コール。
      // Car。
      if (car_after == car_before) {
        path_buf.push_back('a');  // パスをプッシュ。

        core(*(pair.car()));

        path_buf.pop_back();  // パスをポップ。
      }

      // Cdr。
      if (cdr_after == cdr_before) {
        path_buf.push_back('d');  // パスをプッシュ。

        core(*(pair.cdr()));

        path_buf.pop_back();  // パスをポップ。
      }
    };

    // 実行。
    core(pair);
  }

  // マクロ展開する。
  void DevelopMacro(LObject* ptr, const LMacroMap& macro_map) {
    // コールバック関数を作成。
    LFuncForWalk func = [&macro_map](LObject& pair, const std::string& path) {
      // Carを置き換え。
      const LPointer& car = pair.car();
      if (car->IsSymbol()) {
        for (auto& map_pair : macro_map) {
          if (map_pair.first == car->symbol()) {
            pair.car(map_pair.second->Clone());
            break;
          }
        }
      }

      // Cdrを置き換え。
      const LPointer& cdr = pair.cdr();
      if (cdr->IsSymbol()) {
        for (auto& map_pair : macro_map) {
          if (map_pair.first == cdr->symbol()) {
            pair.cdr(map_pair.second->Clone());
            break;
          }
        }
      }
    };

    // Walkする。
    Walk(*ptr, func);
  }

  // 式を評価する。
  LPointer LObject::Evaluate(const LObject& target) {
    // 自分が関数でなければ、何もしない。
    if (!(IsFunction() || IsN_Function())) {
      throw Lisp::GenError("@evaluating-error",
      "Only function object can evaluate expressions.");
    }

    // シンボル、バインドされているオブジェクトを返す。
    if (target.IsSymbol()) {
      const LPointer& result = scope_chain().SelectSymbol(target.symbol());

      // あればクローン。 なければnullptr。
      if (result) return result->Clone();

      throw Lisp::GenError("@evaluating-error",
      "No object is bound to '" + target.symbol() + "'.");
    }

    // ペア、関数呼び出し。
    if (target.IsPair()) {
      // carを評価する。
      LPointer func_obj = Evaluate(*(target.car()));
      if (!func_obj) {
        throw Lisp::GenError("@evaluating-error",
        "Couldn't evaluate function name '" + target.car()->ToString()
        + "'.");
      }

      // ローカルスコープを作る。
      LScopeChain local_chain = func_obj->scope_chain();
      local_chain.AppendNewScope();

      // ネイティブ関数。
      if (func_obj->IsN_Function()) {
        func_obj->scope_chain(local_chain);

        return func_obj->c_function()(func_obj, this, target);
      }

      // リスプの関数。
      if (func_obj->IsFunction()) {
        // 引数リスト。
        const LPointer& arguments = target.cdr();

        // $@のリスト。
        LPointer at_list = Lisp::NewList(Lisp::CountList(*arguments));
        LObject* at_ptr = at_list.get();

        // 引数名。
        const LArgNames& names = func_obj->arg_names();
        LArgNames::const_iterator names_itr = names.begin();
        LArgNames::const_iterator names_end = names.end();

        // マクロマップ。
        LMacroMap macro_map;

        // 各引数を整理。
        // 普通の引数なら評価してバインド。
        // マクロならmacro_mapに登録。
        LPointer result;
        for (LPointer ptr = arguments; ptr->IsPair();
        ptr = ptr->cdr(), Lisp::Next(&at_ptr)) {
          if (names_itr != names_end) {  // 引数名がある。
            if ((*names_itr)[0] == '^') {
              // マクロ名。
              // マクロマップに登録。
              macro_map.push_back(LMacroElm(*names_itr, ptr->car()));
            } else if ((*names_itr)[0] == '&') {
              // マクロ名。 (残りリスト)
              // 残りをマクロマップに入れて抜ける。
              macro_map.push_back(LMacroElm(*names_itr, ptr));
              break;
            } else {
              // 普通の引数名。
              // 評価してバインド。
              result = Evaluate(*(ptr->car()));
              local_chain.InsertSymbol(*names_itr, result);

              // at_listにも登録。
              at_ptr->car(result);
            }

            ++names_itr;
          } else {  // 引数名がない。
            // at_listに登録するだけ。
            at_ptr->car(Evaluate(*(ptr->car())));
          }
        }
        // at_listをスコープにバインド。
        local_chain.InsertSymbol("$@", at_list);

        // names_itrが余っていたらNilをバインド。
        for (; names_itr != names_end; ++names_itr) {
          local_chain.InsertSymbol(*names_itr, Lisp::NewNil());
        }

        // ローカルスコープを関数オブジェクトにセット。
        func_obj->scope_chain(local_chain);

        // もしマクロ引数があったなら、マクロ展開する。
        // マクロがなかったらそのまま。
        LPointerVec expression;
        if (!(macro_map.empty())) {
          LPointer clone;
          for (auto& expr : func_obj->expression()) {
            clone = expr->Clone();

            // マクロ展開。
            if (clone->IsPair()) {
              DevelopMacro(clone.get(), macro_map);
            } else if (clone->IsSymbol()) {
              for (auto& map_pair : macro_map) {
                if (map_pair.first == clone->symbol()) {
                  clone = map_pair.second->Clone();
                  break;
                }
              }
            }

            expression.push_back(clone);
          }
        } else {
          expression = func_obj->expression();
        }

        // 関数呼び出し。
        LPointer ret_ptr = Lisp::NewNil();
        for (auto& expr : expression) {
          ret_ptr = func_obj->Evaluate(*expr);
        }

        if (!ret_ptr) {
          throw Lisp::GenError("@evaluating-error",
          "Failed to execute '" + target.car()->ToString() + "'.");
        }
        return ret_ptr;
      }

      throw Lisp::GenError("@evaluating-error",
      "'" + target.car()->ToString() + "' didn't return function object.");
    }

    // Atomなのでクローンを返す。
    return target.Clone();
  }

  // ======= //
  // LParser //
  // ======= //
  // --- 字句解析器 --- //
  namespace {
    // 制御文字かどうか。
    inline bool IsControlChar(char c) {
      static const std::set<char> control_c {
        '\n', '\r', '\f', '\t', '\v', '\b', '\a', '\0'
      };
      return control_c.find(c) != control_c.end();
    }

    // 開きカッコかどうか。
    inline bool IsOpenParenth(char c) {
      static const std::set<char> open_c {'(', '{', '['};
      return open_c.find(c) != open_c.end();
    }
    // 閉じカッコかどうか。
    inline bool IsCloseParenth(char c) {
      static const std::set<char> close_c {')', '}', ']'};
      return close_c.find(c) != close_c.end();
    }

    // ベクトルに文字列とcをプッシュする。 cが0ならcはプッシュしない。
    inline void PushString(std::queue<std::string>& queue,
    std::ostringstream& oss, char c=0) {
      std::string temp = oss.str();
      if (temp.size()) {
        queue.push(temp);
        oss.str("");
      }

      if (c) queue.push(std::string(1, c));
    }

    // コメントをスキップする。
    inline void SkipComment(std::string::const_iterator& itr,
    const std::string::const_iterator& end_itr) {
      for (; itr != end_itr; ++itr) {
        if (*itr == '\n') return;  // 改行文字は飛ばさない。
      }
    }

    // 文字列トークンを得る。
    std::vector<std::string> ParseStringToken(std::string::const_iterator& itr,
    const std::string::const_iterator& end_itr, bool& in_string) {
      // クオートはない状態で始まる。

      // 準備。
      std::vector<std::string> ret;
      std::ostringstream oss;

      // 文字列をプッシュする関数。
      auto push = [&ret, &oss]() {
        std::string temp = oss.str();
        if (temp.size()) {
          ret.push_back(temp);
          oss.str("");
        }
      };

      // 文字列終了までループ。
      for (; itr != end_itr; ++itr) {
        // 制御文字は無視。
        if (IsControlChar(*itr)) continue;

        // 文字列終了。 クオートは飛ばさない。
        if (*itr == '\"') {
          in_string = false;
          push();
          return ret;
        }

        // エスケープ文字。
        if (*itr == '\\') {
          // 先ず今までのをプッシュ。
          push();

          // バックスラッシュを入れる。
          oss << *itr;
          ++itr;
          if (itr == end_itr) break;

          // バックスラッシュの次の文字を入れる。
          oss << *itr;

          // プッシュ。
          push();
          continue;
        }

        oss << *itr;
      }

      // まだ文字列解析中。
      in_string = true;
      push();
      return ret;
    }

    // コンマをパースする。
    inline std::string ParseComma(std::string::const_iterator& itr,
    const std::string::const_iterator& end_itr) {
      std::ostringstream oss;
      oss << *itr;
      if ((itr + 1) == end_itr) return oss.str();
      if (*(itr + 1) == '@') {
        ++itr;
        oss << *itr;
        return oss.str();
      }
      return oss.str();
    }
  }

  // 字句解析する。
  void LParser::Tokenize(const std::string& code) {
    // イテレータを準備。
    std::string::const_iterator itr = code.begin();
    std::string::const_iterator end_itr = code.end();

    // ストリームの準備。
    std::ostringstream oss;

    // 各文字でループ。
    for (; itr != end_itr; ++itr) {
      // まだ文字列解析中なら文字の解析を続行。
      if (in_string_) {
        for (auto& str : ParseStringToken(itr, end_itr, in_string_)) {
          token_queue_.push(str);
        }
        if (!in_string_) {  // 解析終了なら今はクオートがitr。
          PushString(token_queue_, oss, *itr);
        }

        if (itr == end_itr) break;
        continue;
      }

      // コメント開始。
      if (*itr == ';') {
        // 今までのをプッシュしてコメントを飛ばす。
        PushString(token_queue_, oss);
        SkipComment(itr, end_itr);

        if (itr == end_itr) break;
        continue;
      }

      // 文字列開始。
      if (*itr == '"') {
        // 今までのをプッシュ。
        PushString(token_queue_, oss, *itr);

        // クオートを飛ばす。
        ++itr;

        // 文字列を解析。
        for (auto& str : ParseStringToken(itr, end_itr, in_string_)) {
          token_queue_.push(str);
        }
        if (!in_string_) {  // 解析終了なら今はクオートがitr。
          PushString(token_queue_, oss, *itr);
        }

        if (itr == end_itr) break;
        continue;
      }

      // 空白、制御文字。
      if ((*itr == ' ') || (IsControlChar(*itr))) {
        // 今までのをプッシュするのみ。
        PushString(token_queue_, oss);
        continue;
      }

      // 開きカッコ。
      if (IsOpenParenth(*itr)) {
        // 今までと自身をプッシュする。
        PushString(token_queue_, oss, '(');
        // カッコのカウントを増やす。
        ++parenth_counter_;

        continue;
      }

      // 閉じカッコ。
      if (IsCloseParenth(*itr)) {
        // 今までと自身をプッシュする。
        PushString(token_queue_, oss, ')');
        // カッコのカウントを減らす。
        --parenth_counter_;

        continue;
      }

      // クオートの糖衣構文。
      if (*itr == '\'') {
        // 今までと自身をプッシュする。
        PushString(token_queue_, oss, *itr);
        continue;
      }

      // テンプレートの糖衣構文。
      if (*itr == '`') {
        // 今までと自身をプッシュする。
        PushString(token_queue_, oss, *itr);
        continue;
      }

      // コンマ。
      if (*itr == ',') {
        PushString(token_queue_, oss);
        token_queue_.push(ParseComma(itr, end_itr));
        continue;
      }

      // 普通の文字。
      oss << *itr;
    }

    // 最後にossに残った文字をプッシュして終わる。
    PushString(token_queue_, oss);
  }

  LPointerVec LParser::Parse() {
    LPointerVec ret;

    // きちんとパースできていなければパースしない。
    if ((parenth_counter_ != 0) || (in_string_)) return ret;

    // パースループ。
    while (!(token_queue_.empty())) {
      ret.push_back(ParseCore());
    }

    // 念の為、キューを空にする。
    token_queue_ = std::queue<std::string>();
    return ret;
  }

  LPointer LParser::ParseCore() {
    if (token_queue_.empty()) {
      throw Lisp::GenError("@parse-error", "Token queue is empty.");
    }

    // 最初のトークンを得る。
    std::string front = token_queue_.front();
    token_queue_.pop();

    if (front == "(") {  // リストをパース。
      LPointer ret = Lisp::NewNil();
      LObject* ret_ptr = nullptr;
      while (!(token_queue_.empty())) {
        front = token_queue_.front();

        // リスト終了。
        if (front == ")") {
          token_queue_.pop();
          return ret;
        }

        // ドット対。
        if (front == ".") {
          token_queue_.pop();

          // パース。
          LPointer result = ParseCore();

          if (ret_ptr) {  // retがペア。
            ret_ptr->cdr(result);
          } else {  // retがNil。
            ret = Lisp::NewPair(Lisp::NewNil(), result);
          }

          continue;
        }

        // Carをパース。
        LPointer result = ParseCore();
        Lisp::AppendElementAndGetBack(ret, result, &ret_ptr);
      }
    } else {  // Atomをパース。
      // エラー。
      if ((front == ".") || (front == ")")) {
        throw Lisp::GenError("@parse-error", "Couldn't parse '"
        + front + "'.");
      }

      // quoteの糖衣構文。
      if (front == "'") {
        // quoteシンボルの入ったペア。
        LPointer ret =
        Lisp::NewPair(Lisp::NewSymbol("quote"), Lisp::NewPair());

        // 次をパースする。
        LPointer result = ParseCore();

        // quoteの次にセットして返る。
        ret->cdr()->car(result);
        return ret;
      }

      // backquoteの糖衣構文。
      if (front == "`") {
        // quoteシンボルの入ったペア。
        LPointer ret =
        Lisp::NewPair(Lisp::NewSymbol("backquote"), Lisp::NewPair());

        // 次をパースする。
        LPointer result = ParseCore();

        // quoteの次にセットして返る。
        ret->cdr()->car(result);
        return ret;
      }

      // unquoteの糖衣構文。
      if (front == ",") {
        // quoteシンボルの入ったペア。
        LPointer ret =
        Lisp::NewPair(Lisp::NewSymbol("unquote"), Lisp::NewPair());

        // 次をパースする。
        LPointer result = ParseCore();

        // quoteの次にセットして返る。
        ret->cdr()->car(result);
        return ret;
      }

      // unquote-splicingの糖衣構文。
      if (front == ",@") {
        // quoteシンボルの入ったペア。
        LPointer ret =
        Lisp::NewPair(Lisp::NewSymbol("unquote-splicing"), Lisp::NewPair());

        // 次をパースする。
        LPointer result = ParseCore();

        // quoteの次にセットして返る。
        ret->cdr()->car(result);
        return ret;
      }

      // 文字列。
      if (front == "\"") {
        std::ostringstream oss;
        while (!(token_queue_.empty())) {
          front = token_queue_.front();
          token_queue_.pop();

          if (front == "\"") break;  // 文字列終了なら抜ける。

          if ((front.size() >= 2) && (front[0] == '\\')) {
            // エスケープシーケンスだった場合。
            switch (front[1]) {
              case 'n': oss << '\n'; break;  // 改行。
              case 'r': oss << '\r'; break;  // キャリッジリターン。
              case 'f': oss << '\f'; break;  // 改ページ。
              case 't': oss << '\t'; break;  // タブ。
              case 'v': oss << '\v'; break;  // 垂直タブ。
              case 'a': oss << '\a'; break;  // ベル。
              case 'b': oss << '\b'; break;  // バックスペース。
              case '0': oss << '\0'; break;  // Null文字。
              default: oss << front[1]; break;  // その他。
            }
          } else {
            oss << front;
          }
        }

        return Lisp::NewString(oss.str());
      }

      // 真偽値。 #t。
      if ((front == "#t") || (front == "#T")) {
        return Lisp::NewBoolean(true);
      }

      // 真偽値。 #f。
      if ((front == "#f") || (front == "#F")) {
        return Lisp::NewBoolean(false);
      }

      // 数字かシンボルの判定。
      char c = front[0];
      if ((front.size() >= 2) && ((c == '+') || (c == '-'))) c = front[1];

      // 数字。
      if ((c >= '0') && (c <= '9')) {
        try {
          return Lisp::NewNumber(std::stod(front));
        } catch (...) {
          // エラーならシンボル。
          return Lisp::NewSymbol(front);
        }
      }

      // シンボル。
      return Lisp::NewSymbol(front);
    }

    throw Lisp::GenError("@parse-error", "Couldn't parse '" + front + "'.");
  }


  // ==== //
  // Lisp //
  // ==== //
  // コンストラクタ。
  Lisp::Lisp(const std::vector<std::string>& argv) : LN_Function() {
    c_function_ = *this;
    func_id_ = "Lisp";
    scope_chain_ = LScopeChain();

    SetCoreFunctions();
    SetBasicFunctions();

    // コマンド引数を設定。
    LPointer list = NewList(argv.size());
    LObject* ptr = list.get();
    for (auto& arg : argv) {
      ptr->car(NewString(arg));
      ptr = ptr->cdr().get();
    }
    scope_chain_.InsertSymbol("argv", list);
  }
  // コンストラクタ。
  Lisp::Lisp() : LN_Function() {
    c_function_ = *this;
    func_id_ = "Lisp";
    scope_chain_ = LScopeChain();

    SetCoreFunctions();
    SetBasicFunctions();
    scope_chain_.InsertSymbol("argv", NewNil());
  }
  // コピーコンストラクタ。
  Lisp::Lisp(const Lisp& lisp) : LN_Function(lisp),
  parser_(lisp.parser_) {}
  // ムーブコンストラクタ。
  Lisp::Lisp(Lisp&& lisp) : LN_Function(lisp),
  parser_(std::move(lisp.parser_)) {}
  // コピー代入演算子。
  Lisp& Lisp::operator=(const Lisp& lisp) {
    LN_Function::operator=(lisp);
    parser_ = lisp.parser_;
    return *this;
  }
  // ムーブ代入演算子。
  Lisp& Lisp::operator=(Lisp&& lisp) {
    LN_Function::operator=(lisp);
    parser_ = std::move(lisp.parser_);
    return *this;
  }

  // コア関数を登録する。
  void Lisp::SetCoreFunctions() {
    LC_Function func;
    std::string help;

    func = LC_FUNCTION_OBJ(Help);
    INSERT_LC_FUNCTION(func, "help", "Lisp:help");
    help =
R"...(### help ###

<h6> Usage </h6>

1. `(help)`
2. `(help <String>)`

<h6> Description </h6>

* 1: Returns descriptions of all help.
* 2: Returns a description of `<String>`.

<h6> Example </h6>

    (display (help "car"))
    
    ;; Output
    ;; > ### car ###
    ;; >
    ;; > <h6> Usage </h6>
    ;; >
    ;; >
    ;; > * `(car <List>)`
    ;; >
    ;; > <h6> Description </h6>
    ;; >
    ;; > * Returns the 1st element of `<List>`.
    ;; >
    ;; > <h6> Example </h6>
    ;; >
    ;; >     (display (car (list 111 222 333)))
    ;; >     
    ;; >     ;; Output
    ;; >     ;;
    ;; >     ;; > 111)...";
    help_dict_.emplace("help", help);

    func = LC_FUNCTION_OBJ(Eval);
    INSERT_LC_FUNCTION(func, "eval", "Lisp:eval");
    help =
R"...(### eval ###

<h6> Usage </h6>

* `(eval <Object>)`

<h6> Description </h6>

* Evaluates `<Object>`.

<h6> Example </h6>

    (define x '(+ 1 2 3))
    (display x)
    (display (eval x))
    
    ;; Output
    ;; > (+ 1 2 3)
    ;; > 6))...";
    help_dict_.emplace("eval", help);

    func = LC_FUNCTION_OBJ(ParseFunc);
    INSERT_LC_FUNCTION(func, "parse", "Lisp:parse");
    INSERT_LC_FUNCTION(func, "string->symbol", "Lisp:string->symbol");
    INSERT_LC_FUNCTION(func, "string->number", "Lisp:string->number");
    INSERT_LC_FUNCTION(func, "string->boolean", "Lisp:string->boolean");
    INSERT_LC_FUNCTION(func, "string->list", "Lisp:string->list");
    help =
R"...(### parse ###

<h6> Usage </h6>

* `(parse <S-Expression : String>)`
* `(string->symbol <S-Expression : String>)`
* `(string->number <S-Expression : String>)`
* `(string->boolean <S-Expression : String>)`
* `(string->list <S-Expression : String>)`

<h6> Description </h6>

* Parses `<S-Expression>` and generates a object.

<h6> Example </h6>

    (display (parse "(1 2 3)"))
    
    ;; Output
    ;; > (1 2 3))...";
    help_dict_.emplace("parse", help);
    help_dict_.emplace("string->symbol", help);
    help_dict_.emplace("string->number", help);
    help_dict_.emplace("string->boolean", help);
    help_dict_.emplace("string->list", help);

    func = LC_FUNCTION_OBJ(Parval);
    INSERT_LC_FUNCTION(func, "parval", "Lisp:parval");
    help =
R"...(### parval ###

<h6> Usage </h6>

* `(parse <S-Expression : String>)`

<h6> Description </h6>

* Parses and evaluates `<S-Expression>` and returns result.
    + It is similar to `(eval (parse <S-Expression>))`.

<h6> Example </h6>

    (parval "(display \"Hello\")(display \"World\")")
    
    ;; Output
    ;; > Hello
    ;; > World)...";
    help_dict_.emplace("parval", help);

    func = LC_FUNCTION_OBJ(ToStringFunc);
    INSERT_LC_FUNCTION(func, "to-string", "Lisp:to-string");
    INSERT_LC_FUNCTION(func, "symbol->string", "Lisp:symbol->string");
    INSERT_LC_FUNCTION(func, "number->string", "Lisp:number->string");
    INSERT_LC_FUNCTION(func, "boolean->string", "Lisp:boolean->string");
    INSERT_LC_FUNCTION(func, "list->string", "Lisp:list->string");
    help =
R"...(### to-string ###

<h6> Usage </h6>

* `(to-string <Object>)`
* `(symbol->string <Object>)`
* `(number->string <Object>)`
* `(boolean->string <Object>)`
* `(list->string <Object>)`

<h6> Description </h6>

* Converts `<Object>` to S-Expression as String.

<h6> Example </h6>

    (display (to-string '(1 2 3)))
    
    ;; Output
    ;; > (1 2 3)
    ;;
    
    (display (string? (to-string '(1 2 3))))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("to-string", help);
    help_dict_.emplace("symbol->string", help);
    help_dict_.emplace("number->string", help);
    help_dict_.emplace("boolean->string", help);
    help_dict_.emplace("list->string", help);

    func = LC_FUNCTION_OBJ(Try);
    INSERT_LC_FUNCTION(func, "try", "Lisp:try");
    help =
R"...(### try ###

<h6> Usage </h6>

* `(try (<Try Expr>...) <Catch Expr>...)`

<h6> Description </h6>

* This is Special Form.
    * `<Catch Expr>...` is evaluated if an error have been occurred
      in `<Try Expr>...`.
* Handles exceptions.
* If an exception occurs in `<Try Expr>...`, then
  it stops `<Try Expr>...` and executes `<Catch Expr>...`.
* In a scope of `<Catch Expr>...`, 'exception' symbol is defined.
* Returns a evaluated last object.

<h6> Example </h6>

    (try ((+ 1 "Hello"))
         (display "Error Occured!!"))
    
    ;; Output
    ;; > Error Occured!!
    
    (try ((+ 1 "Hello"))
         (display exception))
    
    ;; Output
    ;; > (@not-number "The 2nd argument of (+) didn't return Number."))...";
    help_dict_.emplace("try", help);

    func = LC_FUNCTION_OBJ(Throw);
    INSERT_LC_FUNCTION(func, "throw", "Lisp:throw");
    help =
R"...(### throw ###

<h6> Usage </h6>

* `(throw <Object>)`

<h6> Description </h6>

* Throws an exception.
* If you use this in (try) function,
  `<Object>` is bound to 'exception' symbol.

<h6> Example </h6>

    (try ((throw 123))
         (display exception))
    
    ;; Output
    ;; > 123)...";
    help_dict_.emplace("throw", help);

    func = LC_FUNCTION_OBJ(CarFunc);
    INSERT_LC_FUNCTION(func, "car", "Lisp:car");
    help =
R"...(### car ###

<h6> Usage </h6>

* `(car <Pair or List>)`

<h6> Description </h6>

* Returns Car value of `<Pair or List>`

<h6> Example </h6>

    (display (car '(111 . 222)))
    ;; Output
    ;; > 111
    
    (display (car (list 111 222 333)))
    
    ;; Output
    ;; > 111)...";
    help_dict_.emplace("car", help);

    func = LC_FUNCTION_OBJ(CdrFunc);
    INSERT_LC_FUNCTION(func, "cdr", "Lisp:cdr");
    help =
R"...(### cdr ###

<h6> Usage </h6>

* `(cdr <Pair or List>)`

<h6> Description </h6>

* Returns Cdr value of `<Pair or List>`

<h6> Example </h6>

    (display (cdr '(111 . 222)))
    ;; Output
    ;; > 222
    
    (display (cdr (list 111 222 333)))
    
    ;; Output
    ;; > (222 333))...";
    help_dict_.emplace("cdr", help);

    func = LC_FUNCTION_OBJ(Cons);
    INSERT_LC_FUNCTION(func, "cons", "Lisp:cons");
    help =
R"...(### cons ###

<h6> Usage </h6>

* `(cons <Object 1> <Object 2>)`

<h6> Description </h6>

* Returns Pair. Car is `<Object 1>`, Cdr is `<Object 2>`.

<h6> Example </h6>


    (display (cons 111 222))
    
    ;; Output
    ;; > (111 . 222)
    
    (display (cons 111 '(222 333)))
    
    ;; Output
    ;; > (111 222 333)
    
    (display (cons 444 (cons 555 (cons 666 ()))))
    
    ;; Output
    ;; > (444 555 666))...";
    help_dict_.emplace("cons", help);

    func = LC_FUNCTION_OBJ(Apply);
    INSERT_LC_FUNCTION(func, "apply", "Lisp:apply");
    help =
R"...(### apply ###

<h6> Usage </h6>

* `(apply <Object 1> <Object 2>)`

<h6> Description </h6>

* Constructs Pair and evaluates it.
  + `<Object 1>` is Car, `<Object 2>` is Cdr.
  + It is same as `(eval (cons <Object 1> <Object 2>))`.

<h6> Example </h6>

    (define a '(1 2 3))
    
    (display (apply '+ a))
    
    ;; Output
    ;; > 6)...";
    help_dict_.emplace("apply", help);

    func = LC_FUNCTION_OBJ(WalkFunc);
    INSERT_LC_FUNCTION(func, "walk", "Lisp:walk");
    help =
R"...(### walk ###

<h6> Usage </h6>

* `(walk <Callback : Function> <Target : Pair>)`

<h6> Description </h6>

* Walks around in `<Target>` and executes `<Callback>`.
    + `<Callback>` receives 2 arguments.
        - 1st : Current element.
        - 2nd : Path. (String)
            - 'a' is Car.
            - 'd' is cdr.
            - For example, "da" means (car (cdr <List>)).
    + If `<Callback>` returns List and its 1st element is `@replace`,
      the current element is replaced for the 2nd element of the list.
* Returns result of `<Target>`.

<h6> Example </h6>

    (define li '(1 2 (3 4) 5))
    (define (my-func elm path)
            (display "elm : " elm " || path : " path)
            (if (equal? elm 3)
                '(@replace "Hello")
                ()))
    
    (display (walk my-func li))
    ;; Output
    ;; > elm : 1 || path : a
    ;; > elm : (2 (3 4) 5) || path : d
    ;; > elm : 2 || path : da
    ;; > elm : ((3 4) 5) || path : dd
    ;; > elm : (3 4) || path : dda
    ;; > elm : (5) || path : ddd
    ;; > elm : 3 || path : ddaa
    ;; > elm : (4) || path : ddad
    ;; > elm : 4 || path : ddada
    ;; > elm : () || path : ddadd
    ;; > elm : 5 || path : ddda
    ;; > elm : () || path : dddd
    ;; > (1 2 ("Hello" 4) 5))...";
    help_dict_.emplace("walk", help);

    func = LC_FUNCTION_OBJ(Quote);
    INSERT_LC_FUNCTION(func, "quote", "Lisp:quote");
    help =
R"...(### quote ###

<h6> Usage </h6>

* `(quote <Object>)`

<h6> Description </h6>

* This is Special Form.
    + `<Object>` is not Evaluated.
* Returns `<Object>` as is.
* Syntactic suger is `'<Object>`

<h6> Example </h6>

    (display (quote (111 222 333)))
    
    ;; Output
    ;; > (111 222 333)
    
    (display '(444 555 666))
    
    ;; Output
    ;; > (444 555 666)
    
    (define x 123)
    (display x)
    (display 'x)
    
    ;; Output
    ;; > 123
    ;; > Symbol: x)...";
    help_dict_.emplace("quote", help);

    func = LC_FUNCTION_OBJ(Backquote);
    INSERT_LC_FUNCTION(func, "backquote", "Lisp:backquote");
    help =
R"...(### backquote ###

<h6> Usage </h6>

* `(backquote <Object>)`

<h6> Description </h6>

* This is Special Form.
    + `<Object>` is treated as template.
        - An object next to `,` or `,@` is evaluated. the others is not.
* Returns an object after template process.
* Syntactic suger is backquote.

<h6> Example </h6>

    (define a 111)
    (define b 222)
    (define c 333)
    (define d '(444 555 666))
    (define e '())
    
    (display (backquote (a b c)))
    (display `(a b c))
    ;; Output
    ;; > (a b c)
    ;; > (a b c)
    
    (display (backquote (a ,b c)))
    (display `(a ,b c))
    ;; Output
    ;; > (a 222 c)
    ;; > (a 222 c)
    
    (display (backquote (a ,d c)))
    (display `(a ,d c))
    ;; Output
    ;; > (a (444 555 666) c)
    ;; > (a (444 555 666) c)
    
    (display (backquote (a ,@d c)))
    (display `(a ,@d c))
    ;; Output
    ;; > (a 444 555 666 c)
    ;; > (a 444 555 666 c)
    
    (display (backquote (a (a ,@d c) c)))
    (display `(a (a ,@d c) c))
    ;; Output
    ;; > (a (a 444 555 666 c) c)
    ;; > (a (a 444 555 666 c) c)
    
    (display (backquote (a ,@e c)))
    (display `(a ,@e c))
    ;; Output
    ;; > (a c)
    ;; > (a c))...";
    help_dict_.emplace("backquote", help);

    func = LC_FUNCTION_OBJ(Lambda);
    INSERT_LC_FUNCTION(func, "lambda", "Lisp:lambda");
    help =
R"...(### lambda ###

<h6> Usage </h6>

* `(lambda (<Args : Symbol>...) <S-Expression>...)`

<h6> Description </h6>

* This is Special Form.
    + All arguments isn't evaluated.
* Returns Function defined by `<S-Expression>...`.
* (lambda) inherits parent's scope and creates its own local scope.
  So using (lambda) in (lambda), you can create closure function.
* `<Args>...` is Symbols as name of arguments.
    + If an argument name is started with `^` or `&`,
      the argument is Macro-Like Argument.

<h6> Example </h6>

    (define myfunc (lambda (x) (+ x 100)))
    (display (myfunc 5))
    ;; Output
    ;; > 105
    
    (define gen-func (lambda (x) (lambda () (+ x 100))))
    (define myfunc2 (gen-func 50))
    (display (myfunc2))
    ;; Output
    ;; > 150
    
    ;; Example of Macro-Like Argument.
    (define gen-func2
      (lambda (^x &y) (lambda () (display (apply '^x '&y)))))
    (define func1 (gen-func2 + 111 222))
    (define func2 (gen-func2 * 333 444 555))
    (func1)
    (func2)
    ;; Output
    ;; > 333
    ;; > 82057860
    (display (to-string func1))
    (display (to-string func2))
    ;; Output
    ;; > (lambda () (display (apply (quote +) (quote (111 222)))))
    ;; > (lambda () (display (apply (quote *) (quote (333 444 555))))))...";
    help_dict_.emplace("lambda", help);

    func = LC_FUNCTION_OBJ(FuncToLambda);
    INSERT_LC_FUNCTION(func, "func->lambda", "Lisp:func->lambda");
    help =
R"...(### func->lambda ###

<h6> Usage </h6>

* `(func->lambda <Function : Function>)`

<h6> Description </h6>

* Converts Function to S-Expression and returns.

<h6> Example </h6>

    (define (myfunc x) (display x))
    (display (func->lambda myfunc))
    ;; Output
    ;; > (lambda (x) (display x)))...";
    help_dict_.emplace("func->lambda", help);

    func = LC_FUNCTION_OBJ(Let);
    INSERT_LC_FUNCTION(func, "let", "Lisp:let");
    help =
R"...(### let ###

<h6> Usage </h6>

* `(let ((<Name : Symbol> <Object>)...) <S-Expression>...)`

<h6> Description </h6>

* This is Special Form.
    + `<Name : Symbol>` isn't evaluated.
    + But `<Object>` and `<S-Expression>` are evaluated.
* Executes `<S-Expression>...` in new local scope.
* (let) inherits parent's scope and creates its own local scope.
  So using (lambda) in (let), you can create closure function.
* `(<Name> <Object>)...` is local values on (let)'s local scope.

<h6> Example </h6>

    (define (gen-func x y) (let ((a x) (b y))
              (lambda () (+ a b))))
    (define myfunc (gen-func 10 20))
    (display (myfunc))
    
    ;; Output
    ;; > 30)...";
    help_dict_.emplace("let", help);

    func = LC_FUNCTION_OBJ(While);
    INSERT_LC_FUNCTION(func, "while", "Lisp:while");
    help =
R"...(### while ###

<h6> Usage </h6>

* `(while <Condition : Boolean> <S-Expression>...)`

<h6> Description </h6>

* This is Special Form.
* While `<Condition>` is #t, it iterates `<S-Expression>...`.
* Returns Object returned by the last S-Expression.

<h6> Example </h6>

    (define i 0)
    (while (< i 5)
        (display "Hello " i)
        (display "World" i)
        (set! i (++ i)))
    
    ;; Output
    ;; > Hello 0
    ;; > World 0
    ;; > Hello 1
    ;; > World 1
    ;; > Hello 2
    ;; > World 2
    ;; > Hello 3
    ;; > World 3
    ;; > Hello 4
    ;; > World 4)...";
    help_dict_.emplace("while", help);

    func = LC_FUNCTION_OBJ(For);
    INSERT_LC_FUNCTION(func, "for", "Lisp:for");
    help =
R"...(### for ###

<h6> Usage </h6>

* `(for (<Variable : Symbol> <List | String>) <S-Expression>...)`

<h6> Description </h6>

* This is Special Form.
    + `<Variable>` is not evaluated.
* Repeats `<S-Expression>...` until a number of elements of `<List | String>`.
    + The element of `<List | String>` is bound to `<Variable>`.
* Returns Object returned by the last S-Expression.

<h6> Example </h6>

    (define aaa '(1 2 3 4 5))
    
    (for (x aaa)
        (display "Hello " x)
        (display "World " (+ x 5)))
    ;; Output
    ;; > Hello 1
    ;; > World 6
    ;; > Hello 2
    ;; > World 7
    ;; > Hello 3
    ;; > World 8
    ;; > Hello 4
    ;; > World 9
    ;; > Hello 5
    ;; > World 10

    (for (x "Hello")
        (display x))
    ;; Output
    ;; > H
    ;; > e
    ;; > l
    ;; > l
    ;; > o)...";
    help_dict_.emplace("for", help);

    func = LC_FUNCTION_OBJ(Define);
    INSERT_LC_FUNCTION(func, "define", "Lisp:define");
    help =
R"...(### define ###

<h6> Usage </h6>

1. `(define <Symbol> <Object>)`
2. `(define (<Name : Symbol> <Args : Symbol>...) <S-Expression>...)`

<h6> Description </h6>

* This is Special Form.
    + 1: `<Symbol>` isn't evaluated.
    + 2: All arguments isn't evaluated.
* Binds something to its scope.
* 1: Binds `<Object>` to `<Symbol>`.
* 2: Defines `<S-Expression>` as Function named `<Name>`,
     and `<Args>...` is names of its arguments.
    + If an argument name is started with `^` or `&`,
      the argument is Macro-Like Argument.

<h6> Example </h6>

    (define x 123)
    (display x)
    
    ;; Output
    ;; > 123
    
    (define (myfunc x) (+ x 10))
    (display (myfunc 5))
    
    ;; Output
    ;; > 15
    
    ;; Example of Macro-Like Argument.
    (define (gen-func ^x &y) (^x &y) (lambda () (display (apply '^x '&y))))
    (define func1 (gen-func + 111 222))
    (define func2 (gen-func * 333 444 555))
    (func1)
    (func2)
    ;; Output
    ;; > 333
    ;; > 82057860
    (display (to-string func1))
    (display (to-string func2))
    ;; Output
    ;; > (lambda () (display (apply (quote +) (quote (111 222)))))
    ;; > (lambda () (display (apply (quote *) (quote (333 444 555))))))...";
    help_dict_.emplace("define", help);

    func = LC_FUNCTION_OBJ(DefineMacro);
    INSERT_LC_FUNCTION(func, "define-macro", "Lisp:define-macro");
    help =
R"...(### define-macro ###

<h6> Usage </h6>

* `(define-macro (<Name : Symbol> <Args : Symbol>...) <S-Expression>...)`

<h6> Description </h6>

* This is Special Form.
    + All arguments isn't evaluated.
* Defines Traditional Macros and returns `<Name>`.
* If an argument name starts with '&', it means the rest of arguments.

<h6> Example </h6>

    (define-macro (my-macro x y &z)
                  (display x)
                  (display y)
                  (display &z)
                  (cons x (cons y &z)))
    
    (display (my-macro * (+ 1 2) 3 4 5))
    ;; Output
    ;; > Symbol: *
    ;; > (+ 1 2)
    ;; > (3 4 5)
    ;; > 180)...";
    help_dict_.emplace("define-macro", help);

    func = LC_FUNCTION_OBJ(Set);
    INSERT_LC_FUNCTION(func, "set!", "Lisp:set!");
    help =
R"...(### set! ###

<h6> Usage </h6>

* `(set! <Symbol> <Object>)`

<h6> Description </h6>

* This is Special Form.
    + `<Symbol>` isn't evaluated.
* Updates `<Symbol>` to `<Object>` on the local scope.

<h6> Example </h6>

    (define x 123)
    (set! x 456)
    (display x)
    
    ;; Output
    ;; > 456
    
    (define myfunc (let ((x 1)) (lambda () (set! x (+ x 1)) x)))
    (display (myfunc))
    (display (myfunc))
    (display (myfunc))
    
    ;; Output
    ;; > 2
    ;; > 3
    ;; > 4)...";
    help_dict_.emplace("set!", help);

    func = LC_FUNCTION_OBJ(If);
    INSERT_LC_FUNCTION(func, "if", "Lisp:if");
    help =
R"...(### if ###

<h6> Usage </h6>

* `(if <Condition : Boolean> <Then> <Else>)`

<h6> Description </h6>

* This is Special Form.
    + Either of `<Then>` and `<Else>` are evaluated.
* If `<Condition>` is true, then (if) evaluates `<Then>`.
  If false, then it evaluates `<Else>`.

<h6> Example </h6>

    (display (if (< 1 2) (+ 3 4) (+ 5 6)))
    
    ;; Output
    ;; > 7)...";
    help_dict_.emplace("if", help);

    func = LC_FUNCTION_OBJ(Cond);
    INSERT_LC_FUNCTION(func, "cond", "Lisp:cond");
    help =
R"...(### cond ###

<h6> Usage </h6>

* `(cond (<Condition : Boolean> <Then>)... (else <Else>))`

<h6> Description </h6>

* This is Special Form.
    + Only one of `<Then>` or `<Else>` are evaluated.
    + `(else <Else>)` is a special list.
* If `<Condition>` is true, then (cond) returns `<Then>`.
  If false, then it evaluates next `<Condition>`.
* If all `<Condition>` are false, then (cond) returns `<Else>`.

<h6> Example </h6>

    (cond
        ((> 1 2) (display "Hello"))
        ((< 3 4) (display "World"))
        (else "Else!!"))
    
    ;; Output
    ;; > World)...";
    help_dict_.emplace("cond", help);

    func = LC_FUNCTION_OBJ(Begin);
    INSERT_LC_FUNCTION(func, "begin", "Lisp:begin");
    help =
R"...(### begin ###

<h6> Usage </h6>

* `(begin <S-Expression>...)`

<h6> Description </h6>

* Executes `<S-Expression>...` in turns and returns last.

<h6> Example </h6>

    (display (begin
                 (display "Hello")
                 (display "World")))
    
    ;; Output
    ;; > Hello
    ;; > World
    ;; > World)...";
    help_dict_.emplace("begin", help);

    func = LC_FUNCTION_OBJ(Display);
    INSERT_LC_FUNCTION(func, "display", "Lisp:display");
    INSERT_LC_FUNCTION(func, "print", "Lisp:print");
    help =
R"...(### display ###

<h6> Usage </h6>

* `(display <Object>...)`
* `(print <Object>...)`

<h6> Description </h6>

* Prints `<Object>` on Standard Output.

<h6> Example </h6>

    (define x 123)
    (display x)
    
    ;; Output
    ;; > 123
    
    (define x 123)
    (display "x is " x)
    
    ;; Output
    ;; > x is 123)...";
    help_dict_.emplace("display", help);
    help_dict_.emplace("print", help);

    func = LC_FUNCTION_OBJ(Stdin);
    INSERT_LC_FUNCTION(func, "stdin", "Lisp:stdin");
    help =
R"...(### stdin ###

<h6> Usage </h6>

* `(stdin <Message Symbol>)`

<h6> Description </h6>

* Returns String from Standard Input.
* `<Message Symbol>` is a message to the input stream.
    + `@get` : Reads one charactor.
    + `@read-line` : Reads one line. ('LF(CR+LF)' is omitted.)
    + `@read` : Reads all.
* If Standard Input is already closed, it returns Nil.

<h6> Example </h6>

    ;; Reads and shows one charactor from Standard Input.
    (display (stdin '@get))
    
    ;; Reads and shows one line from Standard Input.
    (display (stdin '@read-line))
    
    ;; Reads and shows all from Standard Input.
    (display (stdin '@read)))...";
    help_dict_.emplace("stdin", help);

    func = LC_FUNCTION_OBJ(Stdout);
    INSERT_LC_FUNCTION(func, "stdout", "Lisp:stdout");
    help =
R"...(### stdout ###

<h6> Usage </h6>

* `(stdout <String>)`

<h6> Description </h6>

* Prints `<String>` on Standard Output.

<h6> Example </h6>

    (stdout (to-string 123))
    (stdout "\n")
    
    ;; Output
    ;; > 123)...";
    help_dict_.emplace("stdout", help);

    func = LC_FUNCTION_OBJ(Stderr);
    INSERT_LC_FUNCTION(func, "stderr", "Lisp:stderr");
    help =
R"...(### stderr ###

<h6> Usage </h6>

* `(stderr <String>)`

<h6> Description </h6>

* Prints `<String>` on Standard Error.

<h6> Example </h6>

    (stderr (to-string 123))
    (stderr "\n")
    
    ;; Output
    ;; > 123)...";
    help_dict_.emplace("stderr", help);

    func = LC_FUNCTION_OBJ(Import);
    INSERT_LC_FUNCTION(func, "import", "Lisp:import");
    help =
R"...(### import ###

<h6> Usage </h6>

* `(import <File name : String>)`

<h6> Description </h6>

* Reads `<File name>` and executes it.
* Returns the last evaluated Object of `<File name>`.

<h6> Example </h6>

    ;; When the following code is written in 'hello.scm'
    ;;
    ;; (define a 111)
    ;; (define b 222)
    ;; (string-append "Hello " "World")  ;; <- The last S-Expression.
    
    (display (import "hello.scm"))
    (display "a: " a)
    (display "b: " b)
    
    ;; Output
    ;; > Hello World
    :: > a: 111
    :: > b: 222)...";
    help_dict_.emplace("import", help);

    func = LC_FUNCTION_OBJ(Export);
    INSERT_LC_FUNCTION(func, "export", "Lisp:export");
    help =
R"...(### export ###

<h6> Usage </h6>

* `(export <File name : String> <Object>...)`

<h6> Description </h6>

* Converts each `<Object>...` into String and writes it to `<File name>`.
* Returns String written to `<File name>`.

<h6> Example </h6>

    (display (export "aaa.scm" (list '+ 1 2 3) (list 'define 'x "abc")))
    ;; Output
    ;; > (+ 1 2 3)
    ;; > (define x "abc")
    ;;
    ;; In "aaa.scm"
    ;; > (+ 1 2 3)
    ;; > (define x "abc"))...";
    help_dict_.emplace("export", help);

    func = LC_FUNCTION_OBJ(EqualQ);
    INSERT_LC_FUNCTION(func, "equal?", "Lisp:equal?");
    help =
R"...(### equal? ###

<h6> Usage </h6>

* `(equal? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are same structure.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (equal? '(1 2 (3 4) 5) '(1 2 (3 4) 5)))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("import", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::NIL>);
    INSERT_LC_FUNCTION(func, "nil?", "Lisp:nil?");
    INSERT_LC_FUNCTION(func, "null?", "Lisp:null?");
    help =
R"...(### nil? ###

<h6> Usage </h6>

* `(nil? <Object>...)`
* `(null? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Nil.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (nil? ()))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("nil?", help);
    help_dict_.emplace("null?", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::PAIR>);
    INSERT_LC_FUNCTION(func, "pair?", "Lisp:pair?");
    help =
R"...(### pair? ###

<h6> Usage </h6>

* `(pair? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Pair.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (pair? '(1 2 3) '(4 5 6)))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("pair?", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::SYMBOL>);
    INSERT_LC_FUNCTION(func, "symbol?", "Lisp:symbol?");
    help =
R"...(### symbol? ###

<h6> Usage </h6>

* `(symbol? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Symbol.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (symbol? 'x))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("symbol?", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::NUMBER>);
    INSERT_LC_FUNCTION(func, "number?", "Lisp:number?");
    help =
R"...(### number? ###

<h6> Usage </h6>

* `(number? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Number.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (number? 123))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("number?", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::BOOLEAN>);
    INSERT_LC_FUNCTION(func, "boolean?", "Lisp:boolean?");
    help =
R"...(### boolean? ###

<h6> Usage </h6>

* `(boolean? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Boolean.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (boolean? #f))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("boolean?", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::STRING>);
    INSERT_LC_FUNCTION(func, "string?", "Lisp:string?");
    help =
R"...(### string? ###

<h6> Usage </h6>

* `(string? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are String.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (string? "Hello"))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("string?", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::FUNCTION>);
    INSERT_LC_FUNCTION(func, "function?", "Lisp:function?");
    help =
R"...(### function? ###

<h6> Usage </h6>

* `(function? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Function.
  Otherwise, returns #f.

<h6> Example </h6>

    (define myfunc (lambda (x) (+ x 1)))
    (display (function? myfunc))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("function?", help);

    func = LC_FUNCTION_OBJ(QFunc<LType::N_FUNCTION>);
    INSERT_LC_FUNCTION(func, "native-function?", "Lisp:native-function?");
    help =
R"...(### native-function? ###

<h6> Usage </h6>

* `(native-function? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Native Function.
  Otherwise, returns #f.

<h6> Example </h6>

    (display (native-function? +))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("native-function?", help);

    func = LC_FUNCTION_OBJ(ProcedureQ);
    INSERT_LC_FUNCTION(func, "procedure?", "Lisp:procedure?");
    help =
R"...(### procedure? ###

<h6> Usage </h6>

* `(procedure? <Object>...)`

<h6> Description </h6>

* Returns #t if all `<Object>...` are Function or Native Function.
  Otherwise, returns #f.

<h6> Example </h6>

    (define myfunc (lambda (x) (+ x 1)))
    (display (procedure? myfunc))
    
    ;; Output
    ;; > #t
    
    (display (procedure? +))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("procedure?", help);

    func = LC_FUNCTION_OBJ(OutputStream);
    INSERT_LC_FUNCTION(func, "output-stream", "Lisp:output-stream");
    help =
R"...(### output-stream ###

<h6> Usage </h6>

1. `(output-stream <File name : String>)`
2. `((output-stream <File name : String>) <String>)`

<h6> Description </h6>

* 1: Returns Native Function as an output stream of `<File name>`.
* 2: Writes `<String>` to `<File name>` and returns itself.
* If you give Nil to the Function, the stream will be closed.

<h6> Example </h6>

    ;; Opens "hello.txt".
    (define myfile (output-stream "hello.txt"))
    
    ;; Writes "Hello World" to "hello.txt".
    (myfile "Hello World\n")
    
    ;; Closes "hello.txt".
    (myfile ()))...";
    help_dict_.emplace("output-stream", help);

    func = LC_FUNCTION_OBJ(InputStream);
    INSERT_LC_FUNCTION(func, "input-stream", "Lisp:input-stream");
    help =
R"...(### input-stream ###

<h6> Usage </h6>

1. `(input-stream <File name : String>)`
2. `((input-stream <File name : String>) <Message Symbol : Symbol>)`

<h6> Description </h6>

* 1: Returns Native Function as an input stream of `<File name>`.
* 2: Returns String from `<File name>`.
* 2: `<Message Symbol>` is a message to the input stream.
    + `@get` : Reads one charactor.
    + `@read-line` : Reads one line. ('LF(CR+LF)' is omitted.)
    + `@read` : Reads all.
* If you give Nil to the stream, it will be closed.
* If the stream already closed, it returns empty string.

<h6> Example </h6>

    ;; Opens "hello.txt".
    (define myfile (input-stream "hello.txt"))
    
    ;; Reads and shows one charactor from "hello.txt".
    (display (myfile '@get))
    
    ;; Reads and shows one line from "hello.txt".
    (display (myfile '@read-line))
    
    ;; Reads and shows all from "hello.txt".
    (display (myfile '@read))
    
    ;; Closes "hello.txt".
    (myfile ()))...";
    help_dict_.emplace("input-stream", help);

    func = LC_FUNCTION_OBJ(GenThread);
    INSERT_LC_FUNCTION(func, "gen-thread", "Lisp:gen-thread");
    help =
R"...(### gen-thread ###

<h6> Usage </h6>

* `(gen-thread <Function>)`

<h6> Description </h6>

* Returns Thread object.
* It is controlled by Message Symbol.
    + `@start` : Starts `<Function>` on another thread.
    + `@join` : Waits for `<Function>` to be terminated.
    + `@terminated?` : If `<Function>` has already been terminated, returns #t.

<h6> Example </h6>

    ;; Make function object.
    (define (inner-func)
            (display "Hello")
            (sleep 10)
            (display "World"))
    
    ;; Generate Thread object.
    (define my-thread (gen-thread inner-func))
    
    ;; Start new thread.
    (my-thread '@start)
    
    ;; Judge the thread is terminated.
    (sleep 2)
    (display (my-thread '@terminated?))
    
    ;; Wait for the thread.
    (my-thread '@join)
    
    ;; Output
    ;; > Hello
    ;; > #f  ;; the thread has not been terminated yet.
    ;; > World  ;; This word is printed after 10 seconds from "Hello".)...";
    help_dict_.emplace("gen-thread", help);

    func = LC_FUNCTION_OBJ(Sleep);
    INSERT_LC_FUNCTION(func, "sleep", "Lisp:sleep");
    help =
R"...(### sleep ###

<h6> Usage </h6>

* `(sleep <Seconds : Number>)`

<h6> Description </h6>

* Sleeps the current thread for `<Seconds>` seconds.

<h6> Example </h6>

    (display "Hello")
    
    (sleep 10)
    
    (display "World")
    ;; Output
    ;; > Hello
    ;; > World  ;; This word is printed after 10 seconds from "Hello".)...";
    help_dict_.emplace("sleep", help);

    func = LC_FUNCTION_OBJ(GenMutex);
    INSERT_LC_FUNCTION(func, "gen-mutex", "Lisp:gen-mutex");
    help =
R"...(### gen-mutex ###

<h6> Usage </h6>

* `(gen-mutex)`

<h6> Description </h6>

* Generates Mutex object.
* It is controlled by Message Symbol.
    + `@lock` : Locks mutex.
    + `@unlock` : Unlocks mutex.
    + `@synchronize <S-Expressions>...` :
        - Locks mutex and executes `<S-Expressions>...`.
            - If `<S-Expression>...` is ended,
              the mutex is unlocked and returns the last expression.
        - `<S-Expressions>...` is executed on new local scope.
        - `(wait)` is bound in the new local scope.
            - If `(wait)` is called, the mutex is unlocked
              and the thread sleeps until `@notify-one` or `@notify-all`.
    + `@notify-one` : Wakes one thread up at `(wait)`.
    + `@notify-all` : Wakes all threads up at `(wait)`.

<h6> Example </h6>

    ;; --- Test `@lock` and `@unlock` --- ;;
    ;; Generate Mutex.
    (define my-mutex (gen-mutex))
    
    ;; Define function that uses `@lock` and `@unlock`.
    (define (func-1 word)
            (my-mutex '@lock)
            (display "Hello : " word)
            (sleep 10)
            (my-mutex '@unlock))
    
    ;; Generate Threads.
    (define thread-1 (gen-thread (lambda () (func-1 "Apple"))))
    (define thread-2 (gen-thread (lambda () (func-1 "Banana"))))
    
    ;; Start threads.
    (thread-1 '@start)
    (thread-2 '@start)
    
    ;; Wait for threads.
    (thread-1 '@join)
    (thread-2 '@join)
    
    ;; Output
    ;; > Hello : Banana
    ;; > Hello : Apple  ;; This is printed after 10 seconds.
    
    ;; --- Test `@synchronize` --- ;;
    (define (func-2 word)
            (my-mutex '@synchronize
                      (display "Hello : " word)
                      (sleep 10)))
    
    (define thread-3 (gen-thread (lambda () (func-2 "Water"))))
    (define thread-4 (gen-thread (lambda () (func-2 "Fire"))))
    
    (thread-3 '@start)
    (thread-4 '@start)
    
    (thread-3 '@join)
    (thread-4 '@join)
    
    ;; Output
    ;; > Hello : Water
    ;; > Hello : Fire  ;; This is printed after 10 seconds.
    
    ;; --- Test `@notify-one` and `(wait)` --- ;;
    (define (func-3 word)
            (my-mutex '@synchronize
                      (wait)
                      (display "Hello : " word)))
    
    (define thread-5 (gen-thread (lambda () (func-3 "Sun"))))
    (define thread-6 (gen-thread (lambda () (func-3 "Moon"))))
    
    (thread-5 '@start)
    (thread-6 '@start)
    
    ;; Notify each thread.
    (sleep 2)
    (my-mutex '@notify-one)
    (sleep 10)
    (my-mutex '@notify-one)
    
    (thread-5 '@join)
    (thread-6 '@join)
    
    ;; Output
    ;; > Hello : Sun
    ;; > Hello : Moon  ;; This is printed after 10 seconds.
    
    ;; --- Test `@notify-all` and `(wait)` --- ;;
    (define thread-7 (gen-thread (lambda () (func-3 "Mario"))))
    (define thread-8 (gen-thread (lambda () (func-3 "Sonic"))))
    
    (thread-7 '@start)
    (thread-8 '@start)
    
    ;; Notify all threads.
    (sleep 10)
    (my-mutex '@notify-all)
    
    (thread-7 '@join)
    (thread-8 '@join)
    
    ;; Output
    ;; > Hello : Mario
    ;; > Hello : Sonic  ;; This is printed same time.)...";
    help_dict_.emplace("gen-mutex", help);

    func = LC_FUNCTION_OBJ(System);
    INSERT_LC_FUNCTION(func, "system", "Lisp:system");
    help =
R"...(### system ###

<h6> Usage </h6>

* `(system <Command line : String>)`

<h6> Description </h6>

* Executes `<Command line>` and returns its status.
    + This is same as `system()` of libc.

<h6> Example </h6>

    (display (system "echo 'Hello World'"))
    ;; Output
    ;; > Hello World
    ;; > 0)...";
    help_dict_.emplace("system", help);
  }

  // 基本関数を登録する。
  void Lisp::SetBasicFunctions() {
    LC_Function func;
    std::string help;

    func = LC_FUNCTION_OBJ(Append);
    INSERT_LC_FUNCTION(func, "append", "Lisp:append");
    INSERT_LC_FUNCTION(func, "string-append", "Lisp:string-append");
    help =
R"...(### append ###

<h6> Usage </h6>

1. `(append <List> <Object>...)`
2. `(append <String> <Object>...)`
  + or `(string-append <String> <Object>...)`

<h6> Description </h6>

1. If the 1st argument is List, appends `<Object>...` to its Cdr.
2. If the 1st argument is String,
   converts `<Object>...` into String and concatenates them.

<h6> Example </h6>

    (display (append '(111 222) '(333 444) '(555 666) 777))
    
    ;; Output
    ;; > (111 222 333 444 555 666 . 777)
    
    (display (append "Hello " 111 " World"))
    
    ;; Output
    ;; > "Hello 111 World")...";
    help_dict_.emplace("append", help);
    help_dict_.emplace("string-append", help);

    func = LC_FUNCTION_OBJ(Ref);
    INSERT_LC_FUNCTION(func, "ref", "Lisp:ref");
    INSERT_LC_FUNCTION(func, "list-ref", "Lisp:list-ref");
    INSERT_LC_FUNCTION(func, "string-ref", "Lisp:string-ref");
    help =
R"...(### ref ###

<h6> Usage </h6>

1. `(ref <List> <Index : Number>)`
  + or `(list-ref <List> <Index : Number>)`
2. `(ref <String> <Index : Number>)`
  + or `(string-ref <String> <Index : Number>)`

<h6> Description </h6>

1. If the 1st argument is List, returns a element of `<Index>`th `<List>`.
1. If the 1st argument is String, returns a letter of `<Index>`th `<String>`.
* The index of 1st element is 0.
* If `<Index>` is negative number,
  It counts from the tail of `<List | String>`.

<h6> Example </h6>

    (display (ref '(111 222 333) 1))
    
    ;; Output
    ;; > 222
    
    (display (ref '(111 222 333) -1))
    
    ;; Output
    ;; > 333
    
    (display (ref "Hello World" 4))
    
    ;; Output
    ;; > "o"
    
    (display (ref "Hello World" -3))
    
    ;; Output
    ;; > "r")...";
    help_dict_.emplace("ref", help);
    help_dict_.emplace("list-ref", help);
    help_dict_.emplace("string-ref", help);

    func = LC_FUNCTION_OBJ(List);
    INSERT_LC_FUNCTION(func, "list", "Lisp:list");
    help =
R"...(### list ###


<h6> Usage </h6>

* `(list <Object>...)`

<h6> Description </h6>

* Returns List composed of `<Object>...`.

<h6> Example </h6>

    (display (list 111 222 333))
    
    ;; Output
    ;; > (111 222 333))...";
    help_dict_.emplace("list", help);

    func = LC_FUNCTION_OBJ(ListReplace);
    INSERT_LC_FUNCTION(func, "list-replace", "Lisp:list-replace");
    help =
R"...(### list-replace ###

<h6> Usage </h6>

* `(list-replace <List> <Index : Number> <Object>)`

<h6> Description </h6>

* Returns List which has replaced the `<Index>`th element of
  `<List>` for `<Object>`.
* The 1st element of `<List>` is 0.
* If `<Index>` is negative number," It counts from the tail of `<List>`.

<h6> Example </h6>

    (define lst (list 111 222 333))
    (display (list-replace lst 1 "Hello"))
    
    ;; Output
    ;; > (111 "Hello" 333))...";
    help_dict_.emplace("list-replace", help);

    func = LC_FUNCTION_OBJ(ListRemove);
    INSERT_LC_FUNCTION(func, "list-remove", "Lisp:list-remove");
    help =
R"...(### list-remove ###

<h6> Usage </h6>

* `(list-remove <List> <Index : Number>)`

<h6> Description </h6>

* Returns List which has removed the `<Index>`th element of `<List>`.
* The 1st element of `<List>` is 0.
* If `<Index>` is negative number," It counts from the tail of `<List>`.

<h6> Example </h6>

    (define lst (list 111 222 333))
    (display (list-remove lst 1))
    
    ;; Output
    ;; > (111 333))...";
    help_dict_.emplace("list-remove", help);

    func = LC_FUNCTION_OBJ(ListInsert);
    INSERT_LC_FUNCTION(func, "list-insert", "Lisp:list-insert");
    help =
R"...(### list-insert ###

<h6> Usage </h6>

* `(list-insert <List> <Index : Number> <Object>)`

<h6> Description </h6>

* Insert `<Object>` into `<Index>` of `<List>` and returns it.
* The 1st element of `<List>` is 0.
* If `<Index>` is negative number," It counts from the tail of `<List>`.

<h6> Example </h6>

    (display (list-insert '(111 222 333) 0 "Hello World"))
    (display (list-insert '(111 222 333) 1 "Hello World"))
    (display (list-insert '(111 222 333) 2 "Hello World"))
    (display (list-insert '(111 222 333) 3 "Hello World"))
    ;; Output
    ;; > ("Hello World" 111 222 333)
    ;; > (111 "Hello World" 222 333)
    ;; > (111 222 "Hello World" 333)
    ;; > (111 222 333 "Hello World"))...";
    help_dict_.emplace("list-insert", help);

    func = LC_FUNCTION_OBJ(ListSearch);
    INSERT_LC_FUNCTION(func, "list-search", "Lisp:list-search");
    help =
R"...(### list-search ###

<h6> Usage </h6>

* `(list-search <List> <Object>)`

<h6> Description </h6>

* If `<List>` has an object same as `<Object>`,
  it returns index number of the object.  
  Otherwise it returns Nil.

<h6> Example </h6>

    (define lst '(111 222 "Hello" #t))
    
    (display (list-search lst "Hello"))
    (display (list-search lst "World"))
    
    ;; Output
    ;; >  2
    ;; > ())...";
    help_dict_.emplace("list-search", help);

    func = LC_FUNCTION_OBJ(ListPath);
    INSERT_LC_FUNCTION(func, "list-path", "Lisp:list-path");
    help =
R"...(### list-path ###

<h6> Usage </h6>

* `(list-path <List> <Path : String>)`

<h6> Description </h6>

* Returns an element indicated by `<Path>` from `<List>`.
    + `<Path>` is composed of 'a' or 'd'.
        - 'a' means Car.
        - 'b' means Cdr.
        - If `<Path>` is "dadda" and name of List is 'lst',
          it means `(car (cdr (cdr (car (cdr lst)))))`.

<h6> Example </h6>

    (define lst '(111 (222 333 444) 555 666))
    
    (display (list-path lst "dda"))
    ;; Output
    ;; > 555
    
    (display (list-path lst "dada"))
    ;; Output
    ;; > 333)...";
    help_dict_.emplace("list-path", help);

    func = LC_FUNCTION_OBJ(Zip);
    INSERT_LC_FUNCTION(func, "zip", "Lisp:zip");
    help =
R"...(### zip ###

<h6> Usage </h6>

* `(zip <List>...)`

<h6> Description </h6>

* Unions each element of `<List>...` and returns it.

<h6> Example </h6>

    (display (zip '(a b c)
                  '(1 2 3)
                  '("Hello" "World")
                  '("aaa" "bbb" "ccc" "ddd")))
    ;; Output
    ;; > ((a 1 "Hello" "aaa") (b 2 "World" "bbb") (c 3 "ccc") ("ddd")))...";
    help_dict_.emplace("zip", help);

    func = LC_FUNCTION_OBJ(Map);
    INSERT_LC_FUNCTION(func, "map", "Lisp:map");
    help =
R"...(### map ###

<h6> Usage </h6>

* `(map <Function : Symbol or Function> <Argument list : List>...)`

<h6> Description </h6>

* Applies `<Function>` to `<Argument List>...` iteratively
  and returns List composed with returned object by `<Function>`.
      + It looks like addition of Linear Algebra. See Example.

<h6> Example </h6>

    (display (map '+ '(1 2 3 4 5)
                     '(10 20 30 40 50)
                     '(100 200 300 400 500)))
    ;; The above means...
    ;; (display (list (+ 1 10 100)
    ;;                (+ 2 20 200)
    ;;                (+ 3 30 300)
    ;;                (+ 4 40 400)
    ;;                (+ 5 50 500)))
    
    ;; Output
    ;; > (111 222 333 444 555))...";
    help_dict_.emplace("map", help);

    func = LC_FUNCTION_OBJ(Filter);
    INSERT_LC_FUNCTION(func, "filter", "Lisp:filter");
    help =
R"...(### filter ###

<h6> Usage </h6>

* `(filter <Function : Symbol or Function> <List>)`

<h6> Description </h6>

* Judges each element of `<List>` by `<Function>`
  and returns List of the passed elements.
* `<Function>` must return #t or #f.
* `<Function>` accepts one argument.

<h6> Example </h6>

    (define li '(1 2 3 4 5 6))
    (define (my-func x) (> x 3))
    
    (display (filter my-func li))
    ;; Output
    ;; > (4 5 6))...";
    help_dict_.emplace("filter", help);

    func = LC_FUNCTION_OBJ(Range);
    INSERT_LC_FUNCTION(func, "range", "Lisp:range");
    help =
R"...(### range ###

<h6> Usage </h6>

* `(range <Size : Number>)`

<h6> Description </h6>

* Returns List composed with 0...(`<Size>` - 1).

<h6> Example </h6>

    (display (range 10))
    
    ;; Output
    ;; > (0 1 2 3 4 5 6 7 8 9))...";
    help_dict_.emplace("range", help);

    func = LC_FUNCTION_OBJ(StartSizeInc);
    INSERT_LC_FUNCTION(func, "start-size-inc", "Lisp:start-size-inc");
    help =
R"...(### start-size-inc ###

<h6> Usage </h6>

* `(start-size-inc <Start : Number> <Size : Number> <Increment : Number>)`

<h6> Description </h6>

* Returns List of `<Size>` elements.
    + The 1st element is `<Start>`.
    + From the 2nd element, the previous element plus `<Increment>`.

<h6> Example </h6>

    (display (start-size-inc 2 6 -0.5))
    
    ;; Output
    ;; > (2 1.5 1 0.5 0 -0.5))...";
    help_dict_.emplace("start-size-inc", help);

    func = LC_FUNCTION_OBJ(LengthFunc);
    INSERT_LC_FUNCTION(func, "length", "Lisp:length");
    help =
R"...(### length ###

<h6> Usage </h6>

* `(length <Object>)`

<h6> Description </h6>

* If `<Object>` is List, it returns the number of elements.
* If `<Object>` is String, it returns the length of string.
* If `<Object>` is Atom, it returns 1.

<h6> Example </h6>

    (display (length '(111 222 333 444 555 666)))
    ;; Output
    ;; > 6
    
    (display (length "Hello"))
    ;; Output
    ;; > 5)...";
    help_dict_.emplace("length", help);

    func = LC_FUNCTION_OBJ(NumEqual);
    INSERT_LC_FUNCTION(func, "=", "Lisp:=");
    help =
R"...(### = ###

<h6> Usage </h6>

* `(= <Number>...)`

<h6> Description </h6>

* Returns #t if the 1st Number equals the others.
  Otherwise, return #f.

<h6> Example </h6>

    (display (= 111 111 111))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("=", help);

    func = LC_FUNCTION_OBJ(NumNotEqual);
    INSERT_LC_FUNCTION(func, "~=", "Lisp:~=");
    help =
R"...(### ~= ###

<h6> Usage </h6>

* `(~= <Number>...)`

<h6> Description </h6>

* Returns #f if the 1st Number equals the others.
  Otherwise, return #t.

<h6> Example </h6>

    (display (~= 111 111 111))
    
    ;; Output
    ;; > #f)...";
    help_dict_.emplace("~=", help);

    func = LC_FUNCTION_OBJ(NumGT);
    INSERT_LC_FUNCTION(func, ">", "Lisp:>");
    help =
R"...(### > ###

<h6> Usage </h6>

* `(> <Number>...)`

<h6> Description </h6>

* Returns #t if a Number is more than next Number.
  Otherwise, return #f.

<h6> Example </h6>

    (display (> 333 222 111))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace(">", help);

    func = LC_FUNCTION_OBJ(NumGE);
    INSERT_LC_FUNCTION(func, ">=", "Lisp:>=");
    help =
R"...(### >= ###

<h6> Usage </h6>

* `(>= <Number>...)`

<h6> Description </h6>

* Returns #t if a Number is more or equal than next Number.
  Otherwise, return #f.

<h6> Example </h6>

    (display (>= 333 222 111))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace(">=", help);

    func = LC_FUNCTION_OBJ(NumLT);
    INSERT_LC_FUNCTION(func, "<", "Lisp:<");
    help =
R"...(### < ###

<h6> Usage </h6>

* `(< <Number>...)`

<h6> Description </h6>

* Returns #t if a Number is less than next Number.
  Otherwise, return #f.

<h6> Example </h6>

    (display (< 111 222 333))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("<", help);

    func = LC_FUNCTION_OBJ(NumLE);
    INSERT_LC_FUNCTION(func, "<=", "Lisp:<=");
    help =
R"...(### <= ###

<h6> Usage </h6>

* `(<= <Number>...)`

<h6> Description </h6>

* Returns #t if a Number is less or equal than next Number.
  Otherwise, return #f.

<h6> Example </h6>

    (display (< 111 222 333))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("<=", help);

    func = LC_FUNCTION_OBJ(Not);
    INSERT_LC_FUNCTION(func, "not", "Lisp:not");
    help =
R"...(### not ###

<h6> Usage </h6>

* `(not <Boolean>)`

<h6> Description </h6>

* Turns `<Boolean>` to opposite value. #t to #f, #f to #t.

<h6> Example </h6>

    (display (not (= 111 111)))
    
    ;; Output
    ;; > #f)...";
    help_dict_.emplace("not", help);

    func = LC_FUNCTION_OBJ(And);
    INSERT_LC_FUNCTION(func, "and", "Lisp:and");
    help =
R"...(### and ###

<h6> Usage </h6>

* `(and <Boolean>...)`

<h6> Description </h6>

* Returns #t if all `<Boolean>...` are #t.
  Otherwise, return #f.

<h6> Example </h6>

    (display (and (= 111 111) (= 222 222)))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("and", help);

    func = LC_FUNCTION_OBJ(Or);
    INSERT_LC_FUNCTION(func, "or", "Lisp:or");
    help =
R"...(### or ###

<h6> Usage </h6>

* `(or <Boolean>...)`

<h6> Description </h6>

* Returns #t if one of `<Boolean>...` is #t.
  If all `<Boolean>` are #f, return #f.

<h6> Example </h6>

    (display (or (= 111 111) (= 222 333)))
    
    ;; Output
    ;; > #t)...";
    help_dict_.emplace("or", help);

    func = LC_FUNCTION_OBJ(Addition);
    INSERT_LC_FUNCTION(func, "+", "Lisp:+");
    help =
R"...(### + ###

<h6> Usage </h6>

* `(+ <Number>...)`

<h6> Description </h6>

* Sums up all `<Number>...`.

<h6> Example </h6>

    (display (+ 1 2 3))
    
    ;; Output
    ;; > 6)...";
    help_dict_.emplace("+", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(2, "+", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "add!", "Lisp:add!");
    help =
R"...(### add! ###

<h6> Usage </h6>

* `(add! <Symbol> <Number>...)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with Number.
    + `<Symbol>` is not evaluated.
* Sums up `<Number>...` and adds it to `<Symbol>`.
* Returns previous Number bound to `<Symbol>`.

<h6> Example </h6>

    (define n 5)
    (display (add! n 3 4 5))  ;; Returns previous Number.
    (display n)
    
    ;; Output
    ;; > 5
    :: > 17)...";
    help_dict_.emplace("add!", help);

    func = LC_FUNCTION_OBJ(Subtraction);
    INSERT_LC_FUNCTION(func, "-", "Lisp:-");
    help =
R"...(### - ###

<h6> Usage </h6>

* `(- <1st number> <Number>...)`

<h6> Description </h6>

* Subtracts `<Number>...` from `<1st number>`.

<h6> Example </h6>

    (display (- 5 4 3))
    
    ;; Output
    ;; > -2)...";
    help_dict_.emplace("-", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(2, "-", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "sub!", "Lisp:sub!");
    help =
R"...(### sub! ###

<h6> Usage </h6>

* `(sub! <Symbol> <Number>...)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with Number.
    + `<Symbol>` is not evaluated.
* Subtracts `<Number>...` from `<Symbol>` and rewrite it to `<Symbol>`.
* Returns previous Number bound to `<Symbol>`.

<h6> Example </h6>

    (define n 5)
    (display (sub! n 1 2))  ;; Returns previous Number.
    (display n)
    
    ;; Output
    ;; > 5
    ;; > 2)...";
    help_dict_.emplace("sub!", help);

    func = LC_FUNCTION_OBJ(Multiplication);
    INSERT_LC_FUNCTION(func, "*", "Lisp:*");
    help =
R"...(### * ###

<h6> Usage </h6>

* `(* <Number>...)`

<h6> Description </h6>

* Multiplies all `<Number>...`.

<h6> Example </h6>

    (display (* 2 3 4))
    
    ;; Output
    ;; > 24)...";
    help_dict_.emplace("*", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(2, "*", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "mul!", "Lisp:mul!");
    help =
R"...(### mul! ###

<h6> Usage </h6>

* `(mul! <Symbol> <Number>...)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with Number.
    + `<Symbol>` is not evaluated.
* Multiplies `<Symbol>` and `<Number>...` and rewrite it to <Symbol>.
* Returns previous Number bound to `<Symbol>`.

<h6> Example </h6>

    (define n 5)
    (display (mul! n 1 2))  ;; Returns previous Number.
    (display n)
    
    ;; Output
    ;; > 5
    ;; > 10)...";
    help_dict_.emplace("mul!", help);

    func = LC_FUNCTION_OBJ(Division);
    INSERT_LC_FUNCTION(func, "/", "Lisp:/");
    help =
R"...(### / ###

<h6> Usage </h6>

* `(/ <1st number> <Number>...)`

<h6> Description </h6>

* Divides `<1st number>` with `<Number>...`.

<h6> Example </h6>

    (display (/ 32 2 4))
    
    ;; Output
    ;; > 4)...";
    help_dict_.emplace("/", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(2, "/", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "div!", "Lisp:div!");
    help =
R"...(### div! ###

<h6> Usage </h6>

* `(div! <Symbol> <Number>...)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with Number.
    + `<Symbol>` is not evaluated.
* Divides `<Symbol>` with `<Number>...` and rewrites it to `<Symbol>`.
* Returns previous Number bound to `<Symbol>`.

<h6> Example </h6>

    (define n 6)
    (display (div! n 2 3))  ;; Returns previous Number.
    (display n)
    
    ;; Output
    ;; > 6
    ;; > 1)...";
    help_dict_.emplace("div!", help);

    func = LC_FUNCTION_OBJ(Inc);
    INSERT_LC_FUNCTION(func, "++", "Lisp:++");
    help =
R"...(### ++ ###

<h6> Usage </h6>

* `(++ <Number>)`

<h6> Description </h6>

* Adds `<Number>` to '1'.

<h6> Example </h6>

    (display (++ 111))
    
    ;; Output
    ;; > 112)...";
    help_dict_.emplace("++", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(1, "++", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "inc!", "Lisp:inc!");
    help =
R"...(### inc! ###

<h6> Usage </h6>

* `(inc! <Symbol)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with Number.
    + `<Symbol>` is not evaluated.
* Increments `<Symbol>`.
* Returns previous Number bound to `<Symbol>`.

<h6> Example </h6>

    (define i 111)
    (display (inc! i))
    (display i)
    
    ;; Output
    ;; > 111
    ;; > 112)...";
    help_dict_.emplace("inc!", help);

    func = LC_FUNCTION_OBJ(Dec);
    INSERT_LC_FUNCTION(func, "--", "Lisp:--");
    help =
R"...(### -- ###

<h6> Usage </h6>

* `(-- <Number>)`

<h6> Description </h6>

* Subtracts '1' from `<Number>`.

<h6> Example </h6>

    (display (-- 111))
    
    ;; Output
    ;; > 110)...";
    help_dict_.emplace("--", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(1, "--", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "dec!", "Lisp:dec!");
    help =
R"...(### dec! ###

<h6> Usage </h6>

* `(dec! <Symbol)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with Number.
    + `<Symbol>` is not evaluated.
* Decrements `<Symbol>`.
* Returns previous Number bound to `<Symbol>`.

<h6> Example </h6>

    (define i 111)
    (display (dec! i))
    (display i)
    
    ;; Output
    ;; > 111
    ;; > 110)...";
    help_dict_.emplace("dec!", help);

    func = LC_FUNCTION_OBJ(StringSplit);
    INSERT_LC_FUNCTION(func, "string-split", "Lisp:string-split");
    help =
R"...(### string-split ###

<h6> Usage </h6>

* `(string-split <String> <Delim :String>)`

<h6> Description </h6>

* Separates `<Sting>` by `<Delim>` and returns the List.

<h6> Example </h6>

    (display (string-split "aaaaSplit!bbbSplit!ccc" "Split!"))
    ;; Output
    ;; > ("aaa" "bbb" "ccc"))...";
    help_dict_.emplace("string-split", help);

    func = LC_FUNCTION_OBJ(StringJoin);
    INSERT_LC_FUNCTION(func, "string-join", "Lisp:string-join");
    help =
R"...(### string-join ###

<h6> Usage </h6>

* `(string-join <List> <Delim : String>)`

<h6> Description </h6>

* Puts `<Delim>` between each elements of `<List>` and returns it.

<h6> Example </h6>

    (define ls '("Hello" "World" 111 222))
    (display (string-join ls "[Delim!!]"))
    ;; Output
    ;; > Hello[Delim!!]World[Delim!!]111[Delim!!]222)...";
    help_dict_.emplace("string-join", help);

    func = LC_FUNCTION_OBJ(Front);
    INSERT_LC_FUNCTION(func, "front", "Lisp:front");
    help =
R"...(### front ###

<h6> Usage </h6>

* `(front <List>)`

<h6> Description </h6>

* Returns the first element of `<List>`.

<h6> Example </h6>

    (display (front '(111 222 333)))
    
    ;; Output
    ;; > 111)...";
    help_dict_.emplace("front", help);

    func = LC_FUNCTION_OBJ(Back);
    INSERT_LC_FUNCTION(func, "back", "Lisp:back");
    help =
R"...(### back ###

<h6> Usage </h6>

* `(back <List>)`

<h6> Description </h6>

* Returns the last element of `<List>`.

<h6> Example </h6>

    (display (back '(111 222 333)))
    
    ;; Output
    ;; > 333)...";
    help_dict_.emplace("back", help);

    func = LC_FUNCTION_OBJ(PushFront);
    INSERT_LC_FUNCTION(func, "push-front", "Lisp:push-front");
    help =
R"...(### push-front ###

<h6> Usage </h6>

* `(push-front <List> <Object>)`

<h6> Description </h6>

* Returns List added `<Object>` at the first element of `<List>`

<h6> Example </h6>

    (display (push-front '(111 222 333) "Hello"))
    
    ;; Output
    ;; > ("Hello" 111 222 333))...";
    help_dict_.emplace("push-front", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(2, "push-front", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "push-front!", "Lisp:push-front!");
    help =
R"...(### push-front! ###

<h6> Usage </h6>

* `(push-front! <Symbol> <Object>)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with List.
    + `<Symbol>` is not evaluated.
* Appends to the top of List of `<Symbol>`.
* Returns previous List bound to `<Symbol>`.

<h6> Example </h6>

    (define l '(111 222 333))
    (display (push-front! l 444))  ;; Returns previous List.
    (display l)
    
    ;; Output
    ;; > (111 222 333)
    ;; > (444 111 222 333))...";
    help_dict_.emplace("push-front!", help);

    func = LC_FUNCTION_OBJ(PopFront);
    INSERT_LC_FUNCTION(func, "pop-front", "Lisp:pop-front");
    help =
R"...(### pop-front ###

<h6> Usage </h6>

* `(pop-front <List>)`

<h6> Description </h6>

* Returns List removed the first element from `<List>`.

<h6> Example </h6>

    (display (pop-front '(111 222 333)))
    
    ;; Output
    ;; > (222 333))...";
    help_dict_.emplace("pop-front", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(1, "pop-front", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "pop-front!", "Lisp:pop-front!");
    help =
R"...(### pop-front! ###

<h6> Usage </h6>

* `(pop-front! <Symbol>)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with List.
* Pops out the 1st element of List of `<Symbol>`.
* Returns previous List bound to `<Symbol>`.

<h6> Example </h6>

    (define l '(111 222 333))
    (display (pop-front! l))
    (display l)
    
    ;; Output
    ;; > (111 222 333)
    ;; > (222 333))...";
    help_dict_.emplace("pop-front!", help);

    func = LC_FUNCTION_OBJ(PushBack);
    INSERT_LC_FUNCTION(func, "push-back", "Lisp:push-back");
    help =
R"...(### push-back ###

<h6> Usage </h6>

* `(push-back <List> <Object>)`

<h6> Description </h6>

* Returns List added `<Object>` at the last element of `<List>`

<h6> Example </h6>

    (display (push-back '(111 222 333) "Hello"))
    
    ;; Output
    ;; > (111 222 333 "Hello"))...";
    help_dict_.emplace("push-back", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(2, "push-back", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "push-back!", "Lisp:push-back!");
    help =
R"...(### push-back! ###

<h6> Usage </h6>

* `(push-back! <Symbol> <Object>)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with List.
    + `<Symbol>` is not evaluated.
* Appends to the tail of List of `<Symbol>`.
* Returns previous List bound to `<Symbol>`.

<h6> Example </h6>

    (define l '(111 222 333))
    (display (push-back! l 444))
    (display l)
    
    ;; Output
    ;; > (111 222 333)
    ;; > (111 222 333 444))...";
    help_dict_.emplace("push-back!", help);

    func = LC_FUNCTION_OBJ(PopBack);
    INSERT_LC_FUNCTION(func, "pop-back", "Lisp:pop-back");
    help =
R"...(### pop-back ###

<h6> Usage </h6>

* `(pop-back <List>)`

<h6> Description </h6>

* Returns List removed the last element from `<List>`.

<h6> Example </h6>

    (display (pop-back '(111 222 333)))
    
    ;; Output
    ;; > (111 222))...";
    help_dict_.emplace("pop-back", help);

    func =
    [this](LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->ExclamToSet(1, "pop-back", self, caller, args);
    };
    INSERT_LC_FUNCTION(func, "pop-back!", "Lisp:pop-back!");
    help =
R"...(### pop-back! ###

<h6> Usage </h6>

* `(pop-back! <Symbol>)`

<h6> Description </h6>

* This is Special Form.
    + The 1st argument must be Symbol bound with List.
* Pops out the last element of List of `<Symbol>`.
* Returns previous List bound to `<Symbol>`.

<h6> Example </h6>

    (define l '(111 222 333))
    (display (pop-back! l))
    (display l)
    
    ;; Output
    ;; > (111 222 333)
    ;; > (111 222))...";
    help_dict_.emplace("pop-back!", help);

    scope_chain_.InsertSymbol("PI", NewNumber(4.0 * std::atan(1.0)));
    help =
R"...(### PI ###

<h6> Description </h6>

* Circular constant.

<h6> Example </h6>

    (display PI)
    
    ;; Output
    ;; > 3.14159265358979)...";
    help_dict_.emplace("PI", help);

    scope_chain_.InsertSymbol("E", NewNumber(std::exp(1.0)));
    help =
R"...(### E ###

<h6> Description </h6>

* Napier's constant.

<h6> Example </h6>

    (display E)
    
    ;; Output
    ;; > 2.71828182845905)...";
    help_dict_.emplace("E", help);

    func = LC_FUNCTION_OBJ(Sin);
    INSERT_LC_FUNCTION(func, "sin", "Lisp:sin");
    help =
R"...(### sin ###

<h6> Usage </h6>

* `(sin <Number>)`

<h6> Description </h6>

* Sine. A trigonometric function.
* `<Number>` is radian.

<h6> Example </h6>

    (display (sin (/ PI 2)))
    
    ;; Output
    ;; > 1)...";
    help_dict_.emplace("sin", help);

    func = LC_FUNCTION_OBJ(Cos);
    INSERT_LC_FUNCTION(func, "cos", "Lisp:cos");
    help =
R"...(### cos ###

<h6> Usage </h6>

* `(cos <Number>)`

<h6> Description </h6>

* Cosine. A trigonometric function.
* `<Number>` is radian.

<h6> Example </h6>

    (display (cos PI))
    
    ;; Output
    ;; > -1)...";
    help_dict_.emplace("cos", help);

    func = LC_FUNCTION_OBJ(Tan);
    INSERT_LC_FUNCTION(func, "tan", "Lisp:tan");
    help =
R"...(### tan ###

<h6> Usage </h6>

* `(tan <Number>)`

<h6> Description </h6>

* Tangent. A trigonometric function.
* `<Number>` is radian.

<h6> Example </h6>

    (display (tan (/ PI 4)))
    
    ;; Output
    ;; > 1)...";
    help_dict_.emplace("tan", help);

    func = LC_FUNCTION_OBJ(ASin);
    INSERT_LC_FUNCTION(func, "asin", "Lisp:asin");
    help =
R"...(### asin ###

<h6> Usage </h6>

* `(asin <Number>)`

<h6> Description </h6>

* Arc sine. A trigonometric function.
* `<Number>` is sine.

<h6> Example </h6>

    (display (asin 0))
    
    ;; Output
    ;; > 0)...";
    help_dict_.emplace("asin", help);

    func = LC_FUNCTION_OBJ(ACos);
    INSERT_LC_FUNCTION(func, "acos", "Lisp:acos");
    help =
R"...(### acos ###

<h6> Usage </h6>

* `(acos <Number>)`

<h6> Description </h6>

* Arc cosine. A trigonometric function.
* `<Number>` is cosine.

<h6> Example </h6>

    (display (acos 1))
    
    ;; Output
    ;; > 0)...";
    help_dict_.emplace("acos", help);

    func = LC_FUNCTION_OBJ(ATan);
    INSERT_LC_FUNCTION(func, "atan", "Lisp:atan");
    help =
R"...(### atan ###

<h6> Usage </h6>

* `(atan <Number>)`

<h6> Description </h6>

* Arc tangent. A trigonometric function.
* `<Number>` is tangent.

<h6> Example </h6>

    (display (atan 0))
    
    ;; Output
    ;; > 0)...";
    help_dict_.emplace("atan", help);

    func = LC_FUNCTION_OBJ(Sqrt);
    INSERT_LC_FUNCTION(func, "sqrt", "Lisp:sqrt");
    help =
R"...(### sqrt ###

<h6> Usage </h6>

* `(sqrt <Number>)`

<h6> Description </h6>

* Returns square root of `<Number>`.

<h6> Example </h6>

    (display (sqrt 4))
    
    ;; Output
    ;; > 2)...";
    help_dict_.emplace("sqrt", help);

    func = LC_FUNCTION_OBJ(Abs);
    INSERT_LC_FUNCTION(func, "abs", "Lisp:abs");
    help =
R"...(### abs ###

<h6> Usage </h6>

* `(abs <Number>)`

<h6> Description </h6>

* Returns absolute value of `<Number>`.

<h6> Example </h6>

    (display (abs -111))
    
    ;; Output
    ;; > 111)...";
    help_dict_.emplace("abs", help);

    func = LC_FUNCTION_OBJ(Ceil);
    INSERT_LC_FUNCTION(func, "ceil", "Lisp:ceil");
    help =
R"...(### ceil ###

<h6> Usage </h6>

* `(ceil <Number>)`

<h6> Description </h6>

* Rounds up `<Number>` into integral value.

<h6> Example </h6>

    (display (ceil 1.3))
    
    ;; Output
    ;; > 2)...";
    help_dict_.emplace("ceil", help);

    func = LC_FUNCTION_OBJ(Floor);
    INSERT_LC_FUNCTION(func, "floor", "Lisp:floor");
    help =
R"...(### floor ###

<h6> Usage </h6>

* `(floor <Number>)`

<h6> Description </h6>

* Rounds down `<Number>` into integral value.

<h6> Example </h6>

    (display (floor 1.3))
    
    ;; Output
    ;; > 1)...";
    help_dict_.emplace("floor", help);

    func = LC_FUNCTION_OBJ(Round);
    INSERT_LC_FUNCTION(func, "round", "Lisp:round");
    help =
R"...(### round ###

<h6> Usage </h6>

* `(round <Number>)`

<h6> Description </h6>

* Rounds `<Number>` into the nearest integral value.

<h6> Example </h6>

    (display (round 1.5))
    
    ;; Output
    ;; > 2
    
    (display (round 1.49))
    
    ;; Output
    ;; > 1)...";
    help_dict_.emplace("round", help);

    func = LC_FUNCTION_OBJ(Trunc);
    INSERT_LC_FUNCTION(func, "trunc", "Lisp:trunc");
    help =
R"...(### trunc ###

<h6> Usage </h6>

* `(trunc <Number>)`

<h6> Description </h6>

* Truncates after decimal point of `<Number>`.

<h6> Example </h6>

    (display (trunc 1.234))
    
    ;; Output
    ;; > 1
    
    (display (trunc -1.234))
    
    ;; Output
    ;; > -1)...";
    help_dict_.emplace("trunc", help);

    func = LC_FUNCTION_OBJ(Exp);
    INSERT_LC_FUNCTION(func, "exp", "Lisp:exp");
    help =
R"...(### exp ###

<h6> Usage </h6>

* `(exp <Number>)`

<h6> Description </h6>

* Exponent function of `<Number>`. The base is Napier's constant.

<h6> Example </h6>

    (display (exp 1))
    
    ;; Output
    ;; > 2.71828)...";
    help_dict_.emplace("exp", help);

    func = LC_FUNCTION_OBJ(Expt);
    INSERT_LC_FUNCTION(func, "expt", "Lisp:expt");
    help =
R"...(### expt ###

<h6> Usage </h6>

* `(expt <Base> <Exponent>)`
* `(^ <Base> <Exponent>)`

<h6> Description </h6>

* Exponent function of `<Exponent>`. The base is `<Base>`.

<h6> Example </h6>

    (display (expt 2 3))
    
    ;; Output
    ;; > 8)...";
    help_dict_.emplace("expt", help);

    func = LC_FUNCTION_OBJ(Log);
    INSERT_LC_FUNCTION(func, "log", "Lisp:log");
    help =
R"...(### log ###

<h6> Usage </h6>

* `(log <Number>)`
* `(ln <Number>)`

<h6> Description </h6>

* Logarithmic function of `<Number>`. The base is Napier's constant.

<h6> Example </h6>

    (display (log E))
    
    ;; Output
    ;; > 1)...";
    help_dict_.emplace("log", help);

    func = LC_FUNCTION_OBJ(Log2);
    INSERT_LC_FUNCTION(func, "log2", "Lisp:log2");
    help =
R"...(### log2 ###

<h6> Usage </h6>

* `(log2 <Number>)`

<h6> Description </h6>

* Logarithmic function of `<Number>`. The base is 2.

<h6> Example </h6>

    (display (log2 8))
    
    ;; Output
    ;; > 3)...";
    help_dict_.emplace("log2", help);

    func = LC_FUNCTION_OBJ(Log10);
    INSERT_LC_FUNCTION(func, "log10", "Lisp:log10");
    help =
R"...(### log10 ###

<h6> Usage </h6>

* `(log10 <Number>)`

<h6> Description </h6>

* Logarithmic function of `<Number>`. The base is 10.

<h6> Example </h6>

    (display (log10 100))
    
    ;; Output
    ;; > 2)...";
    help_dict_.emplace("log10", help);

    std::shared_ptr<std::mt19937>
    engine_ptr(new std::mt19937(std::chrono::system_clock::to_time_t
    (std::chrono::system_clock::now())));
    func = [this, engine_ptr]
    (LPointer self, LObject* caller, const LObject& args) -> LPointer {
      return this->Random(*engine_ptr, self, caller, args);
    };
    scope_chain_.InsertSymbol("random",
    NewN_Function(func, "Lisp:random", scope_chain_));
    help =
R"...(### random ###

<h6> Usage </h6>

* `(random <Number>)`

<h6> Description </h6>

* Returns random number from 0 to `<Number>`.

<h6> Example </h6>

    (display (random 10))
    
    ;; Output
    ;; > 2.42356
    
    (display (random -10))
    
    ;; Output
    ;; > -7.13453)...";
    help_dict_.emplace("random", help);

    func = LC_FUNCTION_OBJ(Max);
    INSERT_LC_FUNCTION(func, "max", "Lisp:max");
    help =
R"...(### max ###

<h6> Usage </h6>

* `(max <Number>...)`

<h6> Description </h6>

* Returns maximum number of `<Number>...`.

<h6> Example </h6>

    (display (max 1 2 3 4 3 2 1))
    
    ;; Output
    ;; > 4)...";
    help_dict_.emplace("max", help);

    func = LC_FUNCTION_OBJ(Min);
    INSERT_LC_FUNCTION(func, "min", "Lisp:min");
    help =
R"...(### min ###

<h6> Usage </h6>

* `(min <Number>...)`

<h6> Description </h6>

* Returns minimum number of `<Number>...`.

<h6> Example </h6>

    (display (min 4 3 2 1 2 3 4))
    
    ;; Output
    ;; > 1)...";
    help_dict_.emplace("min", help);

    func = LC_FUNCTION_OBJ(RegexSearch);
    INSERT_LC_FUNCTION(func, "regex-search", "Lisp:regex-search");
    help =
R"...(### regex-search ###

<h6> Usage </h6>

* `(regex-search <Regex : String> <Target : String>)`

<h6> Description </h6>

* Search `<Regex>` from `<Target>` and returns List of results.
    + The 1st element of results is match of whole `<Target>`.
    + From the 2nd element, each group.

<h6> Example </h6>

    (define target "Hello World")
    (display (regex-search "(Hel).* (Wor).*" target))
    ;; Output
    ;; > ("Hello World" "Hel" "Wor"))...";
    help_dict_.emplace("regex-search", help);

    func = LC_FUNCTION_OBJ(Now);
    INSERT_LC_FUNCTION(func, "now", "Lisp:now");
    help =
R"...(### now ###

<h6> Usage </h6>

* `(now)`

<h6> Description </h6>

* Returns List of current time.
    + List is `(<Year> <Month> <Day> <Hour> <Minute> <Second>)`.

<h6> Example </h6>

    ;; Run at 2016-6-11 15:26:59.
    (display (now))
    ;; Output
    ;; > (2016 6 11 15 26 59))...";
    help_dict_.emplace("now", help);

    func = LC_FUNCTION_OBJ(Clock);
    INSERT_LC_FUNCTION(func, "clock", "Lisp:clock");
    help =
R"...(### clock ###

<h6> Usage </h6>

* `(clock)`

<h6> Description </h6>

* Returns execution time (seconds).

<h6> Example </h6>

    ;; After 1.29 seconds from program has run.
    (clock)
    ;; Output
    ;; 1.29091)...";
    help_dict_.emplace("clock", help);
  }

  // ============== //
  // ネイティブ関数 //
  // ============== //
  namespace {
    // Define()とLambda()で使う関数オブジェクトを作成する部品。
    inline LPointer GenFunction(const LPointer& names_ptr,
    const LPointer& expr_list_ptr, const LScopeChain& scope_chain) {
      // names_ptrがリストかどうかチェックする。
      Lisp::CheckList(*names_ptr);

      // 引数の名前リストを作成。
      LArgNames arg_names(Lisp::CountList(*names_ptr));
      if (arg_names.size()) {
        LArgNames::iterator itr = arg_names.begin();
        for (LObject* ptr = names_ptr.get(); ptr->IsPair();
        Lisp::Next(&ptr), ++itr) {
          const LPointer& elm_ptr = ptr->car();
          Lisp::CheckType(*elm_ptr, LType::SYMBOL);
          *itr = elm_ptr->symbol();
        }
      }

      // 関数定義を作成。
      LPointerVec expression = Lisp::ListToLPointerVec(*expr_list_ptr);

      // 関数オブジェクトにして返す。
      return Lisp::NewFunction(arg_names, expression, scope_chain);
    }
  }

  // リストをジップする。
  LPointer Lisp::ZipLists(LPointerVec& list_vec) {
    LPointerVec ret_vec;

    LPointerVec temp;
    while (true) {
      temp.clear();

      bool found = false;  // ペア発見フラグ。
      for (auto& list_ptr : list_vec) {
        if (list_ptr->IsPair()) {
          temp.push_back(list_ptr->car());
          list_ptr = list_ptr->cdr();

          found = true;  // ペアを発見したのでマーク。
        }
      }
      if (!found) break;  // ペアが一つもなかったら抜ける。

      ret_vec.push_back(LPointerVecToList(temp));
    }

    return LPointerVecToList(ret_vec);
  }

  // %%% help
  DEF_LC_FUNCTION(Lisp::Help) {
    int length = CountList(args) - 1;
    std::ostringstream oss;
    if (length > 0) {
      // キーを得る。
      LPointer result = caller->Evaluate(*(args.cdr()->car()));
      CheckType(*result, LType::STRING);
      const std::string& help = result->string();

      if (help_dict_.find(help) != help_dict_.end()) {
        oss << help_dict_.at(help);
      }
    } else {
      // 全表示。
      for (auto& pair : help_dict_) {
        oss << pair.second << std::endl;
        oss << std::string(79, '-') << std::endl;
      }
    }

    return NewString(oss.str());
  }

  // %%% parse
  DEF_LC_FUNCTION(Lisp::ParseFunc) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // タイプをチェック。
    LPointer result = caller->Evaluate(*(args_ptr->car()));
    CheckType(*result, LType::STRING);

    // パーサを準備。
    LParser parser;

    // 字句解析する。
    parser.Tokenize(result->string());

    // 解析できたかチェック。
    if ((parser.parenth_counter() != 0) || parser.in_string()) {
      // エラー。
      throw GenError("@parse-error",
      "Couldn't parse '" + args_ptr->car()->ToString() + "'.");
    }

    // パースする。
    LPointerVec parse_result = parser.Parse();
    if (parse_result.size() <= 0) {
      // エラー。
      throw GenError("@parse-error",
      "Couldn't parse '" + args_ptr->car()->ToString() + "'.");
    }

    // 一番最後にパースした式を返す。
    return parse_result.at(parse_result.size() - 1);
  }

  // %%% backquote
  DEF_LC_FUNCTION(Lisp::Backquote) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    std::function<void(LPointer)> core;
    core = [&core, &caller](LPointer pair) {
      LObject* ptr = pair.get();

      // ptr->cdr------------------>cddr
      // |    |
      // car  cdar------->cdadr
      //      |           |
      //      cdaar       cdadar
      //      (unquote)   (object)
      //
      // リストの終端cdrにunqoteがあった場合。
      // ptr->cdr--------->cddr----->Nil
      // |    |            |
      // car  cdar         cddar
      //      (unquote)    (objct)
      LPointer result, temp;

      for (; ptr->IsPair(); Next(&ptr)) {
        const LPointer& car = ptr->car();
        const LPointer& cdr = ptr->cdr();

        // carの処理。
        if (car->IsPair()) {
          temp = NewPair(NewNil(), car);
          core(temp);
          ptr->car(temp->cdr());
        }

        // cdrの処理。
        if (cdr->IsPair()) {
          const LPointer& cdar = cdr->car();
          const LPointer& cddr = cdr->cdr();
          if (cdar->IsPair()) {
            const LPointer& cdaar = cdar->car();
            const LPointer& cdadr = cdar->cdr();
            if (cdaar->IsSymbol() && cdadr->IsPair()) {
              const LPointer& cdadar = cdadr->car();
              if (cdaar->symbol() == "unquote") {
                // コンマ。
                // 評価する。
                cdadr->car(caller->Evaluate(*cdadar));

                // つなぎ替える。
                cdadr->cdr(cddr);
                ptr->cdr(cdadr);
              } else if (cdaar->symbol() == "unquote-splicing") {
                // コンマアット。
                // 評価する。
                result = caller->Evaluate(*cdadar);

                if (result->IsPair()) {
                  // ペア。
                  // つなぎ替える。
                  LObject* result_ptr = result.get();
                  for (; result_ptr->cdr()->IsPair(); Next(&result_ptr)) {
                    continue;
                  }
                  result_ptr->cdr(cddr);

                  ptr->cdr(result);
                } else if (result->IsNil()) {
                  // Nil。 なかったことにする。
                  ptr->cdr(cddr);
                } else {
                  // リストじゃなかったので、コンマの動作。
                  cdadr->car(result);
                  cdadr->cdr(cddr);
                  ptr->cdr(cdadr);
                }
              }
            }
          } else if ((cdar->IsSymbol()) && (cddr->IsPair())) {
            // リストの終端cdrにunquoteがあった場合。 両方同じ処理。
            const LPointer& cddar = cddr->car();
            if ((cdar->symbol() == "unquote")
            || (cdar->symbol() == "unquote-splicing")) {
              ptr->cdr(caller->Evaluate(*cddar));
            }
          }
        }
      }
    };

    // 頭にダミーをつけて実行。
    LPointer temp = NewPair(NewNil(), args_ptr->car()->Clone());
    core(temp);

    return temp->cdr();
  }

  // %%% define
  DEF_LC_FUNCTION(Lisp::Define) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数。
    const LPointer& first_arg = args_ptr->car();

    // シンボルならバインド、ペアなら関数定義。
    if (first_arg->IsSymbol()) {
      // シンボルへのバインド。
      // (define <first_args> <args_ptr->cdr>...)

      // 第2引数以降を評価する。
      LPointer result(nullptr);
      for (Next(&args_ptr); args_ptr->IsPair(); Next(&args_ptr)) {
        result = caller->Evaluate(*(args_ptr->car()));
      }

      // 評価結果をスコープにバインド。
      caller->scope_chain().InsertSymbol(first_arg->symbol(), result);

      return first_arg->Clone();
    } else if (first_arg->IsPair()) {
      // 関数定義。
      // (define (<func_name_ptr> <first_args->cdr>...) <args_ptr->cdr>...)
      // 関数名を得る。
      const LPointer& func_name_ptr = first_arg->car();

      // スコープチェーンを得る。
      const LScopeChain& chain = caller->scope_chain();

      // スコープチェーンにバインド。
      chain.InsertSymbol(func_name_ptr->symbol(),
      GenFunction(first_arg->cdr(), args_ptr->cdr(), chain));

      return func_name_ptr->Clone();
    }

    throw GenTypeError(*first_arg, "Symbol or Pair");
  }

  // %%% define-macro
  DEF_LC_FUNCTION(Lisp::DefineMacro) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数はインターフェイス。
    const LPointer& inter_face_ptr = args_ptr->car();
    CheckType(*inter_face_ptr, LType::PAIR);
    Next(&args_ptr);

    // マクロの名。
    const LPointer& macro_name_ptr = inter_face_ptr->car();
    CheckType(*macro_name_ptr, LType::SYMBOL);
    const std::string& macro_name = macro_name_ptr->symbol();

    // マクロの引数名。
    LPointer args_name_ptr = inter_face_ptr->cdr()->Clone();
    for (LObject* ptr = args_name_ptr.get(); ptr->IsPair(); Next(&ptr)) {
      CheckType(*(ptr->car()), LType::SYMBOL);
    }

    // 第2引数は定義式。
    LPointer expression_ptr = args_ptr->Clone();

    // ローカルスコープを作る。
    LScopeChain local_chain = caller->scope_chain();
    local_chain.AppendNewScope();

    // 関数オブジェクトを作成。
    LC_Function func =
    [macro_name_ptr, args_name_ptr, expression_ptr, local_chain]
    (LPointer self, LObject* caller, const LObject& args) -> LPointer {
      // ローカルスコープに引数を評価せずにバインドしていく。
      LPointer args_ptr = args.cdr();
      LObject* names_ptr = args_name_ptr.get();
      for (; (names_ptr->IsPair()) && (args_ptr->IsPair());
      Next(&names_ptr), args_ptr = args_ptr->cdr()) {
        const std::string& name = names_ptr->car()->symbol();
        if (name[0] == '&') {
          local_chain.InsertSymbol(name, args_ptr->Clone());
          break;
        } else {
          local_chain.InsertSymbol(name, args_ptr->car()->Clone());
        }
      }

      // まだ名前が余っていたらNilをバインド。
      for (; names_ptr->IsPair(); Next(&names_ptr)) {
        local_chain.InsertSymbol(names_ptr->car()->symbol(), NewNil());
      }

      // 自身にローカルスコープをセットして各式を評価して式を得る。
      self->scope_chain(local_chain);
      LPointer result_expr = NewNil();
      for (LObject* expr = expression_ptr.get(); expr->IsPair(); Next(&expr)) {
        result_expr = self->Evaluate(*(expr->car()));
      }

      // 出来上がった式を評価して返す。
      return caller->Evaluate(*result_expr);
    };

    LPointer n_function = NewN_Function(func, "Lisp:define-macro;"
    + std::to_string(reinterpret_cast<std::size_t>(expression_ptr.get())),
    caller->scope_chain());

    // スコープにバインド。
    caller->scope_chain().InsertSymbol(macro_name, n_function);

    return macro_name_ptr->Clone();
  }

  // %%% lambda
  DEF_LC_FUNCTION(Lisp::Lambda) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 関数オブジェクトにして返す。
    return GenFunction(args_ptr->car(), args_ptr->cdr(),
    caller->scope_chain());
  }

  // %%% func->Lambda
  DEF_LC_FUNCTION(Lisp::FuncToLambda) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // 関数オブジェクトを得る。
    LPointer func_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*func_ptr, LType::FUNCTION);

    // 引数名リストを得る。
    const LArgNames& arg_names = func_ptr->arg_names();
    LPointer arg_names_list = NewList(arg_names.size());
    LObject* arg_names_list_ptr = arg_names_list.get();
    for (auto& name : arg_names) {
      arg_names_list_ptr->car(NewSymbol(name));
      Next(&arg_names_list_ptr);
    }

    return NewPair(NewSymbol("lambda"), NewPair(arg_names_list,
    LPointerVecToList(func_ptr->expression())));
  }

  // %%% let
  DEF_LC_FUNCTION(Lisp::Let) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // ローカル変数用スコープチェーンを作って自身にセット。。
    LScopeChain local_chain = caller->scope_chain();
    local_chain.AppendNewScope();
    self->scope_chain(local_chain);

    // ローカル変数をローカルチェーンにバインドしていく。
    const LPointer& first_ptr = args_ptr->car();
    CheckList(*first_ptr);
    LPointer local_pair;
    for (LObject* ptr = first_ptr.get(); ptr->IsPair(); Next(&ptr)) {
      local_pair = ptr->car();
      CheckList(*local_pair);
      if (CountList(*local_pair) < 2) {
        throw GenError("@function-error",
        "'" + local_pair->ToString() + "' doesn't have 2 elements and more.");
      }

      // ローカル変数名。
      const LPointer& local_pair_first = local_pair->car();
      CheckType(*local_pair_first, LType::SYMBOL);

      // ローカル変数の初期値。
      LPointer local_pair_second =
      caller->Evaluate(*(local_pair->cdr()->car()));

      // バインドする。
      local_chain.InsertSymbol(local_pair_first->symbol(), local_pair_second);
    }

    // ローカル変数用スコープチェーンを使って各式を評価。
    LPointer ret_ptr = NewNil();
    for (Next(&args_ptr); args_ptr->IsPair(); Next(&args_ptr)) {
      ret_ptr = self->Evaluate(*(args_ptr->car()));
    }

    return ret_ptr;
  }

  // %%% while
  DEF_LC_FUNCTION(Lisp::While) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // ローカル変数用スコープチェーンを作って自身にセット。。
    LScopeChain local_chain = caller->scope_chain();
    local_chain.AppendNewScope();
    self->scope_chain(local_chain);

    // ループする。
    LPointer ret_ptr = NewNil();
    const LPointer& condition_expr = args_ptr->car();
    Next(&args_ptr);
    while (true) {
      // 条件を評価。 falseなら抜ける。
      LPointer condition = self->Evaluate(*condition_expr);
      CheckType(*condition, LType::BOOLEAN);
      if (!(condition->boolean())) break;

      // 各式を実行。
      for (LObject* ptr = args_ptr; ptr->IsPair(); Next(&ptr)) {
        ret_ptr = self->Evaluate(*(ptr->car()));
      }
    }

    return ret_ptr;
  }

  // %%% for
  DEF_LC_FUNCTION(Lisp::For) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // ローカル変数用スコープチェーンを作って自身にセット。。
    LScopeChain local_chain = caller->scope_chain();
    local_chain.AppendNewScope();
    self->scope_chain(local_chain);

    // ループの範囲の準備をする。
    const LPointer& range_expr = args_ptr->car();
    CheckList(*range_expr);
    if (CountList(*range_expr) < 2) {
      throw GenError("@function-error",
      "'" + range_expr->ToString() + "' doesn't have 2 elements and more.");
    }
    const LPointer& item = range_expr->car();
    LPointer range = caller->Evaluate(*(range_expr->cdr()->car()));
    CheckType(*item, LType::SYMBOL);
    const std::string& item_symbol = item->symbol();

    // リストか文字列の次の要素を取り出す関数と終了判定関数。。
    std::function<LPointer()> get_next_elm;
    std::function<bool()> is_not_end;
    LObject* range_ptr = range.get();
    const std::string& str = range->string();
    unsigned int str_i = 0;
    std::size_t str_size = str.size();
    if (range->IsList()) {
      // リスト用。
      get_next_elm = [&range_ptr]() -> LPointer {
        const LPointer& ret = range_ptr->car();
        Next(&range_ptr);
        return ret;
      };

      is_not_end = [&range_ptr]() -> bool {
        return range_ptr->IsPair();
      };
    } else if (range->IsString()) {
      // 文字列用。
      get_next_elm = [&str, &str_i]() -> LPointer {
        char c = str[str_i++];
        return NewString(std::string(1, c));
      };

      is_not_end = [&str_i, str_size]() -> bool {
        return str_i < str_size;
      };
    } else {
      throw GenTypeError(*range, "List or String");
    }

    // ローカルチェーンにシンボルをバインドする。
    local_chain.InsertSymbol(item_symbol, NewNil());

    // ループする。
    LPointer ret_ptr = NewNil();
    Next(&args_ptr);
    while (is_not_end()) {
      // ローカルチェーンに範囲の要素をバインド。
      local_chain.UpdateSymbol(item_symbol, get_next_elm());

      // 各式を実行。
      for (LObject* ptr_2 = args_ptr; ptr_2->IsPair(); Next(&ptr_2)) {
        ret_ptr = self->Evaluate(*(ptr_2->car()));
      }
    }

    return ret_ptr;
  }

  // %%% cond
  DEF_LC_FUNCTION(Lisp::Cond) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    for (; args_ptr->IsPair(); Next(&args_ptr)) {
      // 条件文リストを得る。
      const LPointer& sentence = args_ptr->car();
      CheckList(*sentence);
      if (CountList(*sentence) < 2) {
        throw GenError("@function-error",
        "'" + sentence->ToString() + "' doesn't have 2 elements and more.");
      }

      // 1要素目がelseかどうか。
      LPointer result;
      const LPointer& sentence_car = sentence->car();
      if (sentence_car->symbol() == "else") {
        result = NewBoolean(true);
      } else {
        result = caller->Evaluate(*sentence_car);
        CheckType(*result, LType::BOOLEAN);
      }

      // resultがtrueなら2要素目を実行して終わる。
      if (result->boolean()) {
        LPointer ret_ptr = NewNil();
        for (LObject* ptr = sentence->cdr().get(); ptr->IsPair();
        Next(&ptr)) {
          ret_ptr = caller->Evaluate(*(ptr->car()));
        }

        return ret_ptr;
      }
    }

    return NewNil();
  }

  // %%% try
  DEF_LC_FUNCTION(Lisp::Try) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第位1引数はエラー検出する式のリスト。
    const LPointer& first_ptr = args_ptr->car();
    CheckList(*first_ptr);

    LPointer ret_ptr = NewNil();

    // try{}で実行していく。
    try {
      for (LObject* ptr = first_ptr.get(); ptr->IsPair(); Next(&ptr)) {
        ret_ptr = caller->Evaluate(*(ptr->car()));
      }
    } catch (LPointer error) {
      // エラー発生。
      // 自分のローカルスコープにエラーをバインドして
      // 第2引数以降を実行していく。
      self->scope_chain().InsertSymbol("exception", error);
      for (Next(&args_ptr); args_ptr->IsPair(); Next(&args_ptr)) {
        ret_ptr = self->Evaluate(*(args_ptr->car()));
      }
    }

    return ret_ptr;
  }

  // %%% apply
  DEF_LC_FUNCTION(Lisp::Apply) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数は関数オブジェクトか関数名。
    LPointer func_ptr = caller->Evaluate(*(args_ptr->car()));

    // 第2引数は引数リスト。
    LPointer args_list_ptr = caller->Evaluate(*(args_ptr->cdr()->car()));
    CheckList(*args_list_ptr);

    // ペアにして評価して返す。
    return caller->Evaluate(LPair(func_ptr, WrapListQuote(args_list_ptr)));
  }

  // %%% walk
  DEF_LC_FUNCTION(Lisp::WalkFunc) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数は関数オブジェクトか関数名。
    LPointer func_ptr = caller->Evaluate(*(args_ptr->car()));

    // 第2引数は探索するペア。
    LPointer pair_ptr = caller->Evaluate(*(args_ptr->cdr()->car()));
    CheckType(*pair_ptr, LType::PAIR);

    // ペアにしておく。
    LPointer func_pair_ptr = NewPair(func_ptr, NewPair(NewNil(), NewPair()));

    // 関数オブジェクトを作成。
    LFuncForWalk func = [func_pair_ptr, &caller]
    (LObject& pair, const std::string& path) {
      // --- Car --- //
      // コールバックを実行する。
      func_pair_ptr->cdr()->car(WrapQuote(pair.car()));
      func_pair_ptr->cdr()->cdr()->car(NewString(path + "a"));
      LPointer result = caller->Evaluate(*func_pair_ptr);

      // 結果の第1要素が@replaceなら置き換え。
      if (result->IsPair()) {
        if (result->car()->symbol() == "@replace") {
          if (result->cdr()->IsPair()) {
            pair.car(result->cdr()->car());
          }
        }
      }

      // --- Cdr --- //
      // コールバックを実行する。
      func_pair_ptr->cdr()->car(WrapQuote(pair.cdr()));
      func_pair_ptr->cdr()->cdr()->car(NewString(path + "d"));
      result = caller->Evaluate(*func_pair_ptr);

      // 結果の第1要素が@replaceなら置き換え。
      if (result->IsPair()) {
        if (result->car()->symbol() == "@replace") {
          if (result->cdr()->IsPair()) {
            pair.cdr(result->cdr()->car());
          }
        }
      }
    };

    // Walkする。
    Walk(*pair_ptr, func);

    // 編集後のpair_ptrを返す。
    return pair_ptr;
  }

  // %%% display
  DEF_LC_FUNCTION(Lisp::Display) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    std::ostringstream oss;
    LPointer result;
    for (; args_ptr->IsPair(); Next(&args_ptr)) {
      result = caller->Evaluate(*(args_ptr->car()));

      // StringとSymbol以外はToString()。
      switch (result->type()) {
        case LType::STRING:
          oss << result->string();
          break;
        case LType::SYMBOL:
          oss << "Symbol: " << result->symbol();
          break;
        default:
          oss << result->ToString();
          break;
      }
    }
    oss << std::endl;

    // 標準出力に表示。
    std::cout << oss.str() << std::flush;

    return NewString(oss.str());
  }

  // %%% stdin
  /** ネイティブ関数 - stdin */
  DEF_LC_FUNCTION(Lisp::Stdin) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // 標準入力が閉じていればNil。
    if (!std::cin) return NewNil();

    // メッセージシンボルを得る。
    LPointer symbol_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*symbol_ptr, LType::SYMBOL);
    const std::string& symbol = symbol_ptr->symbol();

    // メッセージシンボルに合わせて分岐。
    if (symbol == "@read") {
      std::ostringstream oss;
      oss << std::cin.rdbuf();
      return NewString(oss.str());
    }
    if (symbol == "@read-line") {
      std::string input;
      std::getline(std::cin, input);
      return NewString(input);
    }
    if (symbol == "@get") {
      return NewString(std::string(1, std::cin.get()));
    }

    throw GenError("@function-error", "'" + args.car()->ToString()
    + "' understands '@read', '@read-line' or '@get'. Not '" + symbol + "'.");
  }

  // %%% stdout
  /** ネイティブ関数 - stdout */
  DEF_LC_FUNCTION(Lisp::Stdout) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // 標準出力が閉じていればNil。
    if (!std::cout) return NewNil();

    // 出力する文字列を得る。
    LPointer str_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*str_ptr, LType::STRING);

    // 出力。
    std::cout << str_ptr->string() << std::flush;

    return self;
  }

  // %%% stderr
  /** ネイティブ関数 - stderr */
  DEF_LC_FUNCTION(Lisp::Stderr) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // 標準エラーが閉じていればNil。
    if (!std::cerr) return NewNil();

    // 出力する文字列を得る。
    LPointer str_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*str_ptr, LType::STRING);

    // 出力。
    std::cerr << str_ptr->string() << std::flush;

    return self;
  }

  // %%% import
  /** ネイティブ関数 - import */
  DEF_LC_FUNCTION(Lisp::Import) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // ファイル名を読み込む。
    LPointer filename_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*filename_ptr, LType::STRING);

    // ファイルを開く。
    std::ifstream ifs(filename_ptr->string());
    if (!ifs) {
      throw GenError("@function-error", "Couldn't find '"
      + filename_ptr->string() + "'.");
    }

    // 読み込む。
    std::ostringstream oss;
    oss << ifs.rdbuf();

    // パーサを準備。
    LParser parser;

    // 字句解析する。
    parser.Tokenize(oss.str());

    // 解析できたかチェック。
    if ((parser.parenth_counter() != 0) || parser.in_string()) {
      throw GenError("@parse-error",
      "Couldn't parse '" + args_ptr->car()->ToString() + "'.");
    }

    // パースする。
    LPointerVec parse_result = parser.Parse();
    if (parse_result.size() <= 0) {
      throw GenError("@parse-error",
      "Couldn't parse '" + args_ptr->car()->ToString() + "'.");
    }

    // 実行する。
    LPointer ret_ptr = NewNil();
    for (auto& ptr : parse_result) {
      ret_ptr = caller->Evaluate(*ptr);
    }

    return ret_ptr;
  }

  // %%% export
  DEF_LC_FUNCTION(Lisp::Export) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // ファイル名をパース。
    LPointer filename_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*filename_ptr, LType::STRING);

    // ファイルを開く。
    std::ofstream ofs(filename_ptr->string());
    if (!ofs) {
      throw GenError("@function-error",
      "Couldn't open '" + filename_ptr->string() + "'.");
    }

    // 残り引数の評価結果を文字列に変換して文字列ストリームにストック。
    std::ostringstream oss;
    for (Next(&args_ptr); args_ptr->IsPair(); Next(&args_ptr)) {
      oss << caller->Evaluate(*(args_ptr->car()))->ToString() << std::endl;
    }

    // 文字列ストリームの内容をファイルに保存。
    ofs << oss.str();

    // 閉じて終了。
    ofs.close();
    return NewString(oss.str());
  }

  // %%% output-stream
  DEF_LC_FUNCTION(Lisp::OutputStream) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // ファイル名を読み込む。
    LPointer filename_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*filename_ptr, LType::STRING);

    // ストリームをヒープで開く。
    std::shared_ptr<std::ofstream>
    ofs_ptr(new std::ofstream(filename_ptr->string()));
    if (!(*ofs_ptr)) {
      throw GenError("@function-error",
      "Couldn't open '" + filename_ptr->string() + "'.");
    }

    // 関数オブジェクトを作成。
    auto c_function = [ofs_ptr](LPointer self, LObject* caller,
    const LObject& args) -> LPointer {
      // ストリームが閉じていれば終了。
      if (!(*ofs_ptr)) return NewNil();

      // 準備。
      LObject* args_ptr = nullptr;
      GetReadyForFunction(args, 1, &args_ptr);

      // 出力する文字列を得る。 Nilならストリームを閉じる。
      LPointer str_ptr = caller->Evaluate(*(args_ptr->car()));
      if (str_ptr->IsNil()) {
        ofs_ptr->close();
        return self;
      }
      CheckType(*str_ptr, LType::STRING);

      *ofs_ptr << str_ptr->string() << std::flush;

      return self;
    };

    return NewN_Function(c_function, "Lisp:output-stream:"
    + std::to_string(reinterpret_cast<std::size_t>(ofs_ptr.get())),
    caller->scope_chain());
  }

  // %%% input-stream
  DEF_LC_FUNCTION(Lisp::InputStream) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // ファイル名を読み込む。
    LPointer filename_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*filename_ptr, LType::STRING);

    // ストリームをヒープで開く。
    std::shared_ptr<std::ifstream>
    ifs_ptr(new std::ifstream(filename_ptr->string()));
    if (!(*ifs_ptr)) {
      throw GenError("@function-error",
      "Couldn't open '" + filename_ptr->string() + "'.");
    }

    // 関数オブジェクトを作成。
    LC_Function c_function = [ifs_ptr](LPointer self, LObject* caller,
    const LObject& args) -> LPointer {
      // ストリームが閉じていれば終了。
      if (!(*ifs_ptr)) return NewNil();

      // 準備。
      LObject* args_ptr = nullptr;
      GetReadyForFunction(args, 1, &args_ptr);

      // メッセージシンボルを得る。 Nilならストリームを閉じる。
      LPointer symbol_ptr = caller->Evaluate(*(args_ptr->car()));
      if (symbol_ptr->IsNil()) {
        ifs_ptr->close();
        return self;
      }
      CheckType(*symbol_ptr, LType::SYMBOL);
      const std::string& symbol = symbol_ptr->symbol();

      // メッセージシンボルに合わせて分岐。
      if (symbol == "@read") {
        std::ostringstream oss;
        oss << ifs_ptr->rdbuf();
        return NewString(oss.str());
      }
      if (symbol == "@read-line") {
        std::string input;
        std::getline(*ifs_ptr, input);
        return NewString(input);
      }
      if (symbol == "@get") {
        return NewString(std::string(1, ifs_ptr->get()));
      }

      throw GenError("@function-error", "'" + args.car()->ToString()
      + "' understands '@read', '@read-line' or '@get'. Not '"
      + symbol + "'.");
    };

    return NewN_Function(c_function, "Lisp:input-stream:"
    + std::to_string(reinterpret_cast<std::size_t>(ifs_ptr.get())),
    caller->scope_chain());
  }

  // %%% gen-thread
  DEF_LC_FUNCTION(Lisp::GenThread) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // 関数オブジェクトを入手。
    LPointer func_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*func_ptr, LType::FUNCTION);

    // スレッド用ポインタ。
    std::shared_ptr<std::thread> thread_ptr(new std::thread());

    // 関数オブジェクトを作成。
    LC_Function func = [func_ptr, thread_ptr](LPointer self, LObject* caller,
    const LObject& args) -> LPointer {
      // 準備。
      LObject* args_ptr = nullptr;
      GetReadyForFunction(args, 1, &args_ptr);

      // メッセージシンボルを得る。
      LPointer message_symbol = caller->Evaluate(*(args_ptr->car()));
      CheckType(*message_symbol, LType::SYMBOL);
      const std::string& message = message_symbol->symbol();

      Next(&args_ptr);

      // 各メッセージシンボルに対応。
      if (message == "@start") {
        // 進行中なら何もしない。
        if (thread_ptr->joinable()) return NewBoolean(false);

        // 関数オブジェクトをコピー。
        LPointer func_clone = func_ptr->Clone();

        // 関数オブジェクトにローカルスコープを追加。
        LScopeChain local_chain = func_clone->scope_chain();
        local_chain.AppendNewScope();

        // 全引数にNilをバインド。
        for (auto& name : func_clone->arg_names()) {
          local_chain.InsertSymbol(name, NewNil());
        }

        // スコープを再セット。
        func_clone->scope_chain(local_chain);

        // スレッドを起動。
        *thread_ptr = std::thread([func_clone]() {
          const LPointerVec expression = func_clone->expression();
          for (auto& expr : expression) {
            func_clone->Evaluate(*expr);
          }
        });

        return NewBoolean(true);
      }

      if (message == "@join") {
        try {
          thread_ptr->join();
          return NewBoolean(true);
        } catch (std::system_error err) {
          // 無視。
        }
        return NewBoolean(false);
      }

      if (message == "@terminated?") {
        return NewBoolean(!(thread_ptr->joinable()));
      }

      throw GenError("@function-error",
      "'" + message + "' is not Thread's Message Symbol.");
    };

    LPointer ret_ptr = NewN_Function(func, "Lisp:gen-thread:"
    + std::to_string(reinterpret_cast<std::size_t>(thread_ptr.get())),
    caller->scope_chain());

    return ret_ptr;
  }

  // %%% sleep
  DEF_LC_FUNCTION(Lisp::Sleep) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    LPointer seconds_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*seconds_ptr, LType::NUMBER);

    std::chrono::milliseconds sleep_time
    (static_cast<int>(seconds_ptr->number() * 1000.0));
    std::this_thread::sleep_for(sleep_time);

    return seconds_ptr;
  }

  // %%% gen-mutex
  DEF_LC_FUNCTION(Lisp::GenMutex) {
    // 各オブジェクトを作成。
    std::shared_ptr<std::mutex> mutex_ptr(new std::mutex);
    std::shared_ptr<std::condition_variable>
    cond_var_ptr(new std::condition_variable());

    // 関数オブジェクトを作成。
    LC_Function func = [mutex_ptr, cond_var_ptr](LPointer self,
    LObject* caller, const LObject& args) -> LPointer {
      // 準備。
      LObject* args_ptr = nullptr;
      GetReadyForFunction(args, 1, &args_ptr);

      // メッセージシンボルを得る。
      LPointer symbol_ptr = caller->Evaluate(*(args_ptr->car()));
      CheckType(*symbol_ptr, LType::SYMBOL);
      const std::string& message = symbol_ptr->symbol();

      if (message == "@lock") {  // ロック。
        mutex_ptr->lock();
        return NewBoolean(true);
      }

      if (message == "@unlock") {  // アンロック。
        mutex_ptr->unlock();
        return NewBoolean(true);
      }

      if (message == "@synchronize") {
        std::unique_lock<std::mutex> lock(*mutex_ptr);  // ユニークロック。

        // ローカルスコープを作成。
        LScopeChain local_chain = caller->scope_chain();
        local_chain.AppendNewScope();

        // (wait)関数を作成。
        LC_Function wait_func =
        [&lock, &mutex_ptr, &cond_var_ptr](LPointer self,
        LObject* caller, const LObject& args) -> LPointer {
          cond_var_ptr->wait(lock);
          return NewBoolean(true);
        };

        // バインド。
        local_chain.InsertSymbol("wait", NewN_Function(wait_func, "Lisp:wait:"
        + std::to_string(reinterpret_cast<std::size_t>(mutex_ptr.get())),
        local_chain));

        // 自身にローカルチェーンをセット。
        self->scope_chain(local_chain);

        // 各S式を実行。
        LPointer ret_ptr = NewNil();
        for (LObject* ptr = args_ptr->cdr().get(); ptr->IsPair(); Next(&ptr)) {
          const LPointer& car = ptr->car();

          // 自身のスコープで実行。
          ret_ptr = self->Evaluate(*car);
        }

        return ret_ptr;
      }

      if (message == "@notify-one") {
        cond_var_ptr->notify_one();
        return NewBoolean(true);
      }

      if (message == "@notify-all") {
        cond_var_ptr->notify_all();
        return NewBoolean(true);
      }

      throw GenError("@function-error",
      "'" + message + "' is not Mutex's Message Symbol.");
    };

    LPointer ret_ptr = NewN_Function(func, "Lisp:gen-mutex:"
    + std::to_string(reinterpret_cast<std::size_t>(mutex_ptr.get())),
    caller->scope_chain());

    return ret_ptr;
  }

  // %%% append
  DEF_LC_FUNCTION(Lisp::Append) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // くっつける元のやつ。。
    LPointer first_ptr = caller->Evaluate(*(args_ptr->car()));

    // リストと文字列で分岐。
    // リストの場合。
    if (first_ptr->IsList()) {
      // ダミーに入れる。
      LPointer dummy = NewPair(NewNil(), first_ptr);
      LObject* dummy_ptr = dummy.get();

      // 残りのリストを付け加えていく。
      for (Next(&args_ptr); args_ptr->IsPair(); Next(&args_ptr)) {
        // dummy_ptrの最後のペアまでループ。
        for (; dummy_ptr->cdr()->IsPair(); Next(&dummy_ptr)) continue;

        // くっつける。
        dummy_ptr->cdr(caller->Evaluate(*(args_ptr->car())));
      }

      // 返す。
      return dummy->cdr();
    }

    // 文字列の場合。
    if (first_ptr->IsString()) {
      // 連結していく。
      std::ostringstream oss;
      oss << first_ptr->string();

      LPointer result;
      for (Next(&args_ptr); args_ptr->IsPair(); Next(&args_ptr)) {
        result = caller->Evaluate(*(args_ptr->car()));
        switch (result->type()) {
          case LType::STRING:
            oss << result->string();
            break;
          case LType::NIL:
          case LType::PAIR:
          case LType::SYMBOL:
          case LType::NUMBER:
          case LType::BOOLEAN:
            oss << result->ToString();
            break;
          default:
            break;
        }
      }

      return NewString(oss.str());
    }

    throw GenTypeError(*(args.car()), "List or String");
  }

  // %%% ref
  DEF_LC_FUNCTION(Lisp::Ref) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数。 ターゲットを得る。
    LPointer target_ptr = caller->Evaluate(*(args_ptr->car()));

    Next(&args_ptr);

    // 第2引数。 インデックスを得る。
    LPointer index_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*index_ptr, LType::NUMBER);
    int index = index_ptr->number();

    // リストの場合。
    if (target_ptr->IsList()) {
      // インデックスを整理。
      if (index < 0) index = CountList(*target_ptr) + index;

      if (index >= 0) {
        for (LObject* ptr = target_ptr.get(); ptr->IsPair();
        Next(&ptr), --index) {
          if (index <= 0) return ptr->car();
        }
      }

      throw GenError("@function-error", "Index '" + index_ptr->ToString()
      + "' of '" + target_ptr->ToString() + "'is out of range.");
    }

    // 文字列の場合。
    if (target_ptr->IsString()) {
      // インデックスを整理。
      const std::string& target_str = target_ptr->string();
      if (index < 0) index =  target_str.size() + index;

      // 範囲違反。
      if ((index < 0) || (index >= static_cast<int>(target_str.size()))) {
        throw GenError("@function-error", "Index '" + index_ptr->ToString()
        + "' of '" + target_ptr->ToString() + "'is out of range.");
      }

      return NewString(std::string(1, target_str[index]));
    }

    // target_ptrが文字列でもリストでもない。
    throw GenTypeError(*target_ptr, "List or String");
  }

  // %%% list-replace
  DEF_LC_FUNCTION(Lisp::ListReplace) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 3, &args_ptr);

    // 第1引数。 ターゲットのリスト。
    LPointer target_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckList(*target_ptr);
    int length = CountList(*target_ptr);
    Next(&args_ptr);

    // 第2引数。 インデックス。
    LPointer index_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*index_ptr, LType::NUMBER);
    int index = index_ptr->number();
    if (index < 0) index = length + index;
    if ((index < 0) || (index >= length)) {
      throw GenError("@function-error", "Index '" + index_ptr->ToString()
      + "' of '" + target_ptr->ToString() + "'is out of range.");
    }
    Next(&args_ptr);

    // 第3引数。 入れ替えたい要素。
    LPointer elm_ptr = caller->Evaluate(*(args_ptr->car()));

    // ダミーに入れる。
    LPointer dummy = NewPair(NewNil(), target_ptr);
    LObject* dummy_ptr = dummy.get();

    // indexが0になるまでシフトする。
    for (; index > 0; --index) Next(&dummy_ptr);

    // 入れ替える。
    dummy_ptr->cdr()->car(elm_ptr);

    // 返す。
    return dummy->cdr();
  }

  // %%% list-remove
  DEF_LC_FUNCTION(Lisp::ListRemove) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数。 ターゲットのリスト。
    LPointer target_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckList(*target_ptr);
    int length = CountList(*target_ptr);
    Next(&args_ptr);

    // 第2引数。 インデックス。
    LPointer index_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*index_ptr, LType::NUMBER);
    int index = index_ptr->number();
    if (index < 0) index = length + index;
    if ((index < 0) || (index >= length)) {
      throw GenError("@function-error", "Index '" + index_ptr->ToString()
      + "' of '" + target_ptr->ToString() + "'is out of range.");
    }

    // ダミー要素を先頭につける。
    LPointer temp = NewPair(NewNil(), target_ptr);
    LObject* ptr = temp.get();

    // 削除する前の要素を特定する。
    for (int i = 0; i < index; ++i) Next(&ptr);

    // 削除する。
    ptr->cdr(ptr->cdr()->cdr());

    // tempのCdrが戻り値。
    return temp->cdr();
  }

  // %%% list-insert
  DEF_LC_FUNCTION(Lisp::ListInsert) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 3, &args_ptr);

    // 第1引数。 ターゲットのリスト。
    LPointer target_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckList(*target_ptr);
    int length = CountList(*target_ptr);
    Next(&args_ptr);

    // 第2引数。 インデックス。
    LPointer index_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*index_ptr, LType::NUMBER);
    int index = index_ptr->number();
    if (index < 0) index = length + index;
    if ((index < 0) || (index > length)) {  // insertの使用上、Lengthも含む。
      throw GenError("@function-error", "Index '" + index_ptr->ToString()
      + "' of '" + target_ptr->ToString() + "'is out of range.");
    }
    Next(&args_ptr);

    // 第3引数。 挿入するオブジェクト。
    LPointer obj_ptr = caller->Evaluate(*(args_ptr->car()));

    // 挿入するオブジェクトをPairにしておく。
    LPointer obj_pair = NewPair(obj_ptr, NewNil());

    // ターゲットの先頭にダミーを入れる。。
    LPointer temp = NewPair(NewNil(), target_ptr);

    // 場所を特定する。
    LObject* ptr = temp.get();
    for (int i = 0; i < index; ++i) Next(&ptr);

    // 付け替える。
    // 先ず自分のCdrにptrのCdrを入れる。
    obj_pair->cdr(ptr->cdr());
    // ptrのCdrに自分を入れる。
    ptr->cdr(obj_pair);

    // tempのCdrが戻り値。
    return temp->cdr();
  }

  // %%% list-search
  DEF_LC_FUNCTION(Lisp::ListSearch) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数。 ターゲットのリスト。
    LPointer target_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckList(*target_ptr);
    Next(&args_ptr);

    // 第2引数。 検索語。
    LPointer elm_ptr = caller->Evaluate(*(args_ptr->car()));

    // インデックスを得る。
    int index = 0;
    for (LObject* ptr = target_ptr.get(); ptr->IsPair();
    Next(&ptr), ++index) {
      if (*(ptr->car()) == *elm_ptr) return NewNumber(index);
    }

    return NewNil();
  }

  // %%% list-path
  DEF_LC_FUNCTION(Lisp::ListPath) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数。 ターゲットのリスト。
    LPointer target_ptr = caller->Evaluate(*(args_ptr->car()));
    Next(&args_ptr);

    // 第2引数。 パス。
    LPointer path_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*path_ptr, LType::STRING);

    // パスに合わせて探索する。
    for (auto c : path_ptr->string()) {
      CheckList(*target_ptr);

      if (c == 'a') {
        target_ptr = target_ptr->car();
      } else if (c == 'd') {
        target_ptr = target_ptr->cdr();
      } else {
        throw GenError("@function-error", "'" + std::string(1, c)
        + "' is not path charactor. A charactor must be 'a' or 'd'.");
      }
    }

    return target_ptr;
  }

  // %%% zip
  DEF_LC_FUNCTION(Lisp::Zip) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 1, &args_ptr);

    // 引数を評価してベクトルに入れる。
    LPointerVec args_vec(CountList(*args_ptr));
    LPointerVec::iterator args_itr = args_vec.begin();
    for (; args_ptr->IsPair(); Next(&args_ptr), ++args_itr) {
      *args_itr = caller->Evaluate(*(args_ptr->car()));
    }

    return ZipLists(args_vec);
  }

  // %%% map
  DEF_LC_FUNCTION(Lisp::Map) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数は関数オブジェクトか関数名シンボル。
    LPointer func_ptr = caller->Evaluate(*(args_ptr->car()));

    // 関数をペアのCarに入れておく。
    LPair func_pair(func_ptr, NewPair());

    // 第2引数以降のベクトル。
    Next(&args_ptr);
    LPointerVec args_vec(CountList(*args_ptr));
    LPointerVec::iterator args_itr = args_vec.begin();
    for (; args_ptr->IsPair(); Next(&args_ptr), ++args_itr) {
      *args_itr = caller->Evaluate(*(args_ptr->car()));
    }

    // 引数をジップする。
    LPointer zip = ZipLists(args_vec);

    // マップしていく。
    LPointerVec ret_vec;
    for (LObject* ptr = zip.get(); ptr->IsPair(); Next(&ptr)) {
      // 関数のペアのCdrに引数リストを入れる。
      func_pair.cdr(WrapListQuote(ptr->car()));

      // 評価してベクトルにプッシュ。
      ret_vec.push_back(caller->Evaluate(func_pair));
    }

    return LPointerVecToList(ret_vec);
  }

  // %%% filter
  DEF_LC_FUNCTION(Lisp::Filter) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数は関数オブジェクトか関数名シンボル。
    LPointer func_ptr = caller->Evaluate(*(args_ptr->car()));

    // 関数をリストのCarに入れておく。
    LPair func_pair(func_ptr, NewPair(NewNil(), NewNil()));

    // 第2引数はフィルタに掛けるリスト。
    LPointer target_list = caller->Evaluate(*(args_ptr->cdr()->car()));
    CheckList(*target_list);

    // フィルターしていく。
    LPointer result;
    LPointerVec ret_vec;
    for (LObject* ptr = target_list.get(); ptr->IsPair(); Next(&ptr)) {
      // 関数のリストの第1引数に入れる。
      func_pair.cdr()->car(WrapQuote(ptr->car()));

      // 評価してみる。
      result = caller->Evaluate(func_pair);
      CheckType(*result, LType::BOOLEAN);

      // 評価結果がtrueならリストに追加。
      if (result->boolean()) ret_vec.push_back(ptr->car());
    }

    return LPointerVecToList(ret_vec);
  }

  // %%% start-size-inc
  DEF_LC_FUNCTION(Lisp::StartSizeInc) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 3, &args_ptr);

    // スタートの数字。
    LPointer result = caller->Evaluate((*args_ptr->car()));
    CheckType(*result, LType::NUMBER);
    double start = result->number();
    Next(&args_ptr);

    // 個数。
    result = caller->Evaluate(*(args_ptr->car()));
    CheckType(*result, LType::NUMBER);
    int num = result->number();
    if (num < 0) {
      throw GenError("@function-error",
      "The 2nd argument must be 0 and more.");
    }
    Next(&args_ptr);

    // インクリメント。
    result = caller->Evaluate(*(args_ptr->car()));
    CheckType(*result, LType::NUMBER);
    double inc = result->number();

    // リストを作成する。
    LPointer ret_ptr = NewList(num);
    for (LObject* ptr = ret_ptr.get(); ptr->IsPair();
    Next(&ptr), start += inc) {
      ptr->car(NewNumber(start));
    }

    return ret_ptr;
  }

  // %%% string-split
  DEF_LC_FUNCTION(Lisp::StringSplit) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数。 ターゲット。
    LPointer result = caller->Evaluate(*(args_ptr->car()));
    CheckType(*result, LType::STRING);
    std::string target_str = result->string();

    // 第2引数。 区切り文字列。
    result = caller->Evaluate(*(args_ptr->cdr()->car()));
    CheckType(*result, LType::STRING);
    const std::string& delim_str = result->string();

    // 区切ってプッシュしていく。
    LPointerVec ret_vec;
    for (std::string::size_type pos = target_str.find(delim_str);
    pos != std::string::npos;
    pos = target_str.find(delim_str)) {
      ret_vec.push_back(NewString(target_str.substr(0, pos)));
      target_str = target_str.substr(pos + delim_str.size());
    }
    ret_vec.push_back(NewString(target_str));

    return LPointerVecToList(ret_vec);
  } 

  // %%% string-join
  DEF_LC_FUNCTION(Lisp::StringJoin) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 第1引数。 リスト。
    LPointer list_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckList(*list_ptr);
    Next(&args_ptr);

    // リストが空ならそのまま返す。
    if (list_ptr->IsNil()) return list_ptr;

    // 第2引数。 区切り文字。
    LPointer delim_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*delim_ptr, LType::STRING);
    const std::string& delim = delim_ptr->string();

    // 間に挟んでいく。
    // まずは第1要素。
    std::ostringstream oss;
    if (list_ptr->car()->IsString()) oss << list_ptr->car()->string();
    else oss << list_ptr->car()->ToString();

    // 第2要素以降。
    for (LObject* ptr = list_ptr->cdr().get(); ptr->IsPair(); Next(&ptr)) {
      oss << delim;
      if (ptr->car()->IsString()) oss << ptr->car()->string();
      else oss << ptr->car()->ToString();
    }

    return NewString(oss.str());
  }

  // %%% regex-search
  DEF_LC_FUNCTION(Lisp::RegexSearch) {
    // 準備。
    LObject* args_ptr = nullptr;
    GetReadyForFunction(args, 2, &args_ptr);

    // 正規表現を得る。
    LPointer reg_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*reg_ptr, LType::STRING);
    Next(&args_ptr);
    std::regex reg(reg_ptr->string(), std::regex_constants::ECMAScript);

    // 対象文字列を得る。
    LPointer str_ptr = caller->Evaluate(*(args_ptr->car()));
    CheckType(*str_ptr, LType::STRING);

    // マッチしたものを探す。
    LPointerVec ret_vec;
    std::smatch result;
    if (std::regex_search(str_ptr->string(), result, reg)) {
      for (auto& token : result) {
        ret_vec.push_back(NewString(token));
      }
    }

    return LPointerVecToList(ret_vec);
  }

  // %%% now
  DEF_LC_FUNCTION(Lisp::Now) {
    std::time_t time;
    std::time(&time);
    std::tm* time_st = std::localtime(&time);

    LPointer ret_ptr = NewList(6);
    LObject* ptr = ret_ptr.get();
    ptr->car(NewNumber(time_st->tm_year + 1900));
    Next(&ptr);
    ptr->car(NewNumber(time_st->tm_mon + 1));
    Next(&ptr);
    ptr->car(NewNumber(time_st->tm_mday));
    Next(&ptr);
    ptr->car(NewNumber(time_st->tm_hour));
    Next(&ptr);
    ptr->car(NewNumber(time_st->tm_min));
    Next(&ptr);
    ptr->car(NewNumber(time_st->tm_sec));

    return ret_ptr;
  }
}  // namespace Sayuri
