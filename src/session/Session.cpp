#include "session/Session.h"
#include "transport/PtyTransport.h"
#include "transport/LoopbackTransport.h"

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
      alt_doc_(std::make_unique<AltScreenDocument>()),
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
    if (!bytes.empty())
        transport_->Write(bytes);
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
void Session::OnEnterAltScreen()                 { /* alt-screen not yet implemented */           }
void Session::OnExitAltScreen()                  { /* alt-screen not yet implemented */           }

void Session::AddDocumentListener(IDocumentListener* listener)
{
    main_doc_->AddListener(listener);
}

void Session::RemoveDocumentListener(IDocumentListener* listener)
{
    main_doc_->RemoveListener(listener);
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
    const unsigned short ptyCols = widePty_ ? ptyLineWidth_ : cols;
    transport_->Resize(ptyCols, rows);
}

} // namespace term::session
