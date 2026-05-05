#include "session/Session.h"
#include "transport/PtyTransport.h"
#include "transport/LoopbackTransport.h"
#include <algorithm>

namespace term::session {

// static
std::unique_ptr<transport::Transport> Session::MakeTransport(
    transport::ITransportTarget& target,
    const Connection& conn,
    unsigned short cols,
    unsigned short rows)
{
    return std::visit([&](auto&& desc) -> std::unique_ptr<transport::Transport> {
        using T = std::decay_t<decltype(desc)>;
        if constexpr (std::is_same_v<T, PtyDesc>)
            return std::make_unique<transport::PtyTransport>(target, desc.shell, cols, rows);
        else
            return std::make_unique<transport::LoopbackTransport>(target);
    }, conn.transport);
}

Session::Session(const Connection& conn,
                 int scrollbackLines,
                 unsigned short cols,
                 unsigned short rows,
                 std::function<void()> onDisconnect,
                 unsigned short ptyLineWidth,
                 bool widePty,
                 bool wordWrap)
    : transport_(MakeTransport(*this, conn, widePty ? ptyLineWidth : cols, rows)),
      parser_(*this),
      main_doc_(std::make_unique<MainScreenDocument>(scrollbackLines)),
      alt_doc_(std::make_unique<AltScreenDocument>(rows, cols)),
      active_doc_(main_doc_.get()),
      docLayout_(std::make_unique<DocLayout>(*main_doc_, cols, rows)),
      onDisconnect_(std::move(onDisconnect)),
      lastCols_(cols),
      lastRows_(rows),
      ptyLineWidth_(ptyLineWidth),
      widePty_(widePty)
{
    docLayout_->SetWordWrap(wordWrap);
    transport_->Start();
}

Session::~Session()
{
    transport_->Stop();
}

void Session::Stop()
{
    transport_->Stop();
}

void Session::OnInput(const input::KeyEvent& event)
{
    auto bytes = encoder_.Encode(event);
    if (!bytes.empty()) {
        // Any keypress re-enables auto-scroll so the viewport follows output.
        docLayout_->ScrollToEnd();
        transport_->Write(bytes);
    }
}

void Session::OnData(const std::string& data)
{
    parser_.Process(data);
}

void Session::OnDisconnect()
{
    if (onDisconnect_)
        onDisconnect_();
}

void Session::OnAppendInsertChar(char32_t ch)
{
    active_doc_->AppendInsertChar(ch);
}

void Session::OnBackspace()
{
    active_doc_->Backspace();
}

void Session::OnNewLine()
{
    active_doc_->NewLine();
}

void Session::OnCarriageReturn()
{
    active_doc_->CarriageReturn();
}

void Session::OnSetStyle(const Style& style)
{
    active_doc_->SetCurrentStyle(style);
}

void Session::OnSetTitle(const std::string& title)
{
    active_doc_->SetTitle(title);
}

void Session::OnCursorUp(int count)              { active_doc_->MoveCursorUp(count);              }
void Session::OnCursorDown(int count)            { active_doc_->MoveCursorDown(count);            }
void Session::OnCursorRight(int count)           { active_doc_->MoveCursorRight(count);           }
void Session::OnCursorLeft(int count)            { active_doc_->MoveCursorLeft(count);            }
void Session::OnEraseInLine(int mode)            { active_doc_->EraseInLine(mode);                }
void Session::OnCursorPosition(int row, int col) { active_doc_->MoveCursorToPosition(row, col);   }
void Session::OnCursorToLineStart()              { active_doc_->MoveCursorToLineStart();          }
void Session::OnCursorEnd()                      { active_doc_->MoveCursorToLineEnd();            }
void Session::OnEraseInDisplay(int mode)         { active_doc_->EraseInDisplay(mode);             }
void Session::OnDeleteChar(int count)            { active_doc_->DeleteChar(count);                }
void Session::OnReverseIndex()                   { active_doc_->ReverseIndex();                   }
void Session::OnSetScrollRegion(int top, int bot){ active_doc_->SetScrollRegion(top, bot);        }
void Session::OnInsertLines(int count)           { active_doc_->InsertLines(count);               }
void Session::OnDeleteLines(int count)           { active_doc_->DeleteLines(count);               }
void Session::OnCursorColumnAbsolute(int col)    { active_doc_->MoveCursorToColumn(col);          }
void Session::OnCursorRowAbsolute(int row)       { active_doc_->MoveCursorToRow(row);             }
void Session::OnSaveCursor()                     { active_doc_->SaveCursor();                     }
void Session::OnRestoreCursor()                  { active_doc_->RestoreCursor();                  }
void Session::OnEraseChar(int count)             { active_doc_->EraseChar(count);                 }
void Session::OnInsertChar(int count)            { active_doc_->InsertChar(count);                }
void Session::OnSetInsertMode(bool on)           { active_doc_->SetInsertMode(on);                }
void Session::OnEnterAltScreen()
{
    alt_doc_->Resize(lastRows_, lastCols_);
    for (auto* l : externalListeners_)
        main_doc_->RemoveListener(l);
    active_doc_ = alt_doc_.get();
    docLayout_->SetDocument(*alt_doc_);
    for (auto* l : externalListeners_)
        alt_doc_->AddListener(l);
    if (widePty_)
        transport_->Resize(lastCols_, lastRows_);
    altScreenActive_ = true;
}

void Session::OnExitAltScreen()
{
    for (auto* l : externalListeners_)
        alt_doc_->RemoveListener(l);
    active_doc_ = main_doc_.get();
    docLayout_->SetDocument(*main_doc_);
    for (auto* l : externalListeners_)
        main_doc_->AddListener(l);
    if (widePty_)
        transport_->Resize(ptyLineWidth_, lastRows_);
    altScreenActive_ = false;
}

void Session::AddDocumentListener(IDocumentListener* listener)
{
    active_doc_->AddListener(listener);
    externalListeners_.push_back(listener);
}

void Session::RemoveDocumentListener(IDocumentListener* listener)
{
    active_doc_->RemoveListener(listener);
    externalListeners_.erase(
        std::remove(externalListeners_.begin(), externalListeners_.end(), listener),
        externalListeners_.end());
}

DocLayout& Session::GetDocLayout()
{
    return *docLayout_;
}

void Session::SetTopRow(int row)
{
    docLayout_->SetTopRow(row);
}

void Session::SetViewportSize(unsigned short cols, unsigned short rows)
{
    docLayout_->SetViewportSize(static_cast<int>(cols), static_cast<int>(rows));
    if (cols == lastCols_ && rows == lastRows_) return;
    lastCols_ = cols;
    lastRows_ = rows;
    if (altScreenActive_) {
        alt_doc_->Resize(rows, cols);
        transport_->Resize(cols, rows);
    } else {
        transport_->Resize(widePty_ ? ptyLineWidth_ : cols, rows);
    }
}

} // namespace term::session
