// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/bitnamespage.h>

#include <qt/clientmodel.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QInputMethodEvent>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using NodeRpc::Args;
using NodeRpc::Item;
using NodeRpc::Text;

namespace {
const char* const GREEN{"#10b981"};
const char* const ORANGE{"#f59e0b"};
const char* const GREY{"#6b7280"};
const char* const PURPLE{"#8b5cf6"};
const char* const RED{"#ef4444"};

QLabel* Dim(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setWordWrap(true);
    label->setEnabled(false);
    return label;
}

QLabel* Heading(const QString& text, QWidget* parent)
{
    auto* label{new QLabel(text, parent)};
    label->setObjectName("sectionHeading");
    return label;
}

QLabel* Pill(const QString& text, const char* colour, QWidget* parent)
{
    auto* pill{new QLabel(text, parent)};
    pill->setStyleSheet(QStringLiteral("color: white; background: %1; border-radius: 8px; padding: 1px 8px; font-size: 8pt; font-weight: bold;").arg(QLatin1String(colour)));
    return pill;
}

/** A value as text: objects and arrays as JSON. */
QString Show(const UniValue& value) { return value.isObject() || value.isArray() ? QString::fromStdString(value.write()) : Text(value); }
QString Field(const UniValue& data, const char* key) { return data.isObject() && data.exists(key) ? Show(data[key]) : QString{}; }

/** The names this page reserved and will register, per wallet: "name" or "name\tpublic". */
QString PendingKey(const QString& wallet) { return QStringLiteral("BitNames/pending/%1").arg(wallet); }

const QStringList RECORD_TYPES{QStringLiteral("A"), QStringLiteral("AAAA"), QStringLiteral("CNAME"), QStringLiteral("TXT"), QStringLiteral("MX"),
                               QStringLiteral("SRV"), QStringLiteral("NS"), QStringLiteral("TLSA"), QStringLiteral("DELEGATE")};

/**
 * Item views that leave input method events to their editors. Under Wayland (WSL), an input method
 * event that reaches a view starts an edit, the edit moves the focus, and moving the focus sends
 * the view another such event: the program runs out of stack.
 */
class Table : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;

protected:
    void inputMethodEvent(QInputMethodEvent* event) override { event->ignore(); }
};

class List : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    void inputMethodEvent(QInputMethodEvent* event) override { event->ignore(); }
};

QTableWidget* SimpleTable(const QStringList& headers, QWidget* parent)
{
    auto* table{new Table(0, headers.size(), parent)};
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    // Under Wayland (WSL), an input method event that reaches the view starts an edit, which moves the
    // focus, which sends another: the program runs out of stack. The editors keep their input methods.
    table->setAttribute(Qt::WA_InputMethodEnabled, false);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    return table;
}

/** A list to pick from: nothing in it is edited. */
QListWidget* PickList(QWidget* parent)
{
    auto* list{new List(parent)};
    list->setObjectName("nameList");
    list->setMinimumWidth(220);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setAttribute(Qt::WA_InputMethodEnabled, false);
    return list;
}

/** A row of the records table: host, type (a list to pick from), value, priority. */
void AddRecordRow(QTableWidget* table, const QString& host, const QString& type, const QString& value, const QString& priority, bool editable)
{
    const int row{table->rowCount()};
    table->insertRow(row);
    auto* host_item{new QTableWidgetItem(host)};
    auto* value_item{new QTableWidgetItem(value)};
    auto* priority_item{new QTableWidgetItem(priority)};
    if (editable) {
        auto* combo{new QComboBox(table)};
        combo->addItems(RECORD_TYPES);
        combo->setCurrentText(type);
        table->setCellWidget(row, 1, combo);
    } else {
        auto* type_item{new QTableWidgetItem(type)};
        for (auto* item : {host_item, type_item, value_item, priority_item}) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 1, type_item);
    }
    table->setItem(row, 0, host_item);
    table->setItem(row, 2, value_item);
    table->setItem(row, 3, priority_item);
}

/** A part of the panel that opens and closes. */
QWidget* Foldable(const QString& title, QWidget* content, QWidget* parent)
{
    auto* box{new QWidget(parent)};
    auto* layout{new QVBoxLayout(box)};
    layout->setContentsMargins(0, 6, 0, 0);
    auto* toggle{new QToolButton(box)};
    toggle->setText(title);
    toggle->setCheckable(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(Qt::RightArrow);
    toggle->setAutoRaise(true);
    toggle->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; border: none; }"));
    layout->addWidget(toggle);
    content->setParent(box);
    content->setVisible(false);
    layout->addWidget(content);
    QObject::connect(toggle, &QToolButton::toggled, content, [toggle, content](bool open) {
        content->setVisible(open);
        toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    return box;
}
} // namespace

BitNamesPage::BitNamesPage(NodeRpc::WalletNameFn wallet_name, QWidget* parent)
    : QWidget(parent), m_wallet_name{std::move(wallet_name)}
{
    setStyleSheet(QStringLiteral(
        "QLabel#pageTitle { font-size: 18pt; font-weight: bold; }"
        "QLabel#nameTitle { font-size: 20pt; font-weight: bold; }"
        "QLabel#sectionHeading { font-size: 11pt; font-weight: bold; margin-top: 10px; }"
        "QPushButton#getButton { font-weight: bold; }"
        "QLineEdit#nameSearch { font-size: 14pt; padding: 4px; }"
        "QLabel#availability { font-size: 11pt; font-weight: bold; }"
        "QListWidget#nameList::item { padding: 6px; }"));
    auto* layout{new QVBoxLayout(this)};

    // The title, and whether .x names resolve on this computer.
    auto* top{new QHBoxLayout};
    auto* title{new QLabel(tr("Names"), this)};
    title->setObjectName("pageTitle");
    top->addWidget(title);
    top->addStretch();
    m_dns_chip = new QPushButton(this);
    m_dns_chip->setFlat(true);
    m_dns_chip->setCursor(Qt::PointingHandCursor);
    connect(m_dns_chip, &QPushButton::clicked, this, &BitNamesPage::showDnsHelp);
    top->addWidget(m_dns_chip);
    layout->addLayout(top);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(createMineTab(), tr("My names"));
    m_tabs->addTab(createExploreTab(), tr("Explore"));
    m_tabs->addTab(createMessagesTab(), tr("Messages"));
    layout->addWidget(m_tabs, 1);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { refresh(); });
    auto* timer{new QTimer(this)};
    connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible()) refresh();
    });
    timer->start(5000);
}

void BitNamesPage::setClientModel(ClientModel* client_model) { m_client_model = client_model; }

void BitNamesPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

QWidget* BitNamesPage::createMineTab()
{
    auto* split{new QSplitter(Qt::Horizontal, this)};
    split->setChildrenCollapsible(false);
    auto* left{new QWidget(split)};
    auto* left_layout{new QVBoxLayout(left)};
    left_layout->setContentsMargins(0, 0, 0, 0);
    auto* get{new QPushButton(tr("+  Get a new name"), left)};
    get->setObjectName("getButton");
    get->setMinimumHeight(34);
    left_layout->addWidget(get);
    m_mine = PickList(left);
    left_layout->addWidget(m_mine, 1);
    split->addWidget(left);
    connect(get, &QPushButton::clicked, this, [this] {
        m_mine->clearSelection();
        m_mine->setCurrentItem(nullptr);
        m_mine_shown = QStringLiteral("get");
        showGetPanel();
    });
    m_mine_detail = new QScrollArea(split);
    m_mine_detail->setWidgetResizable(true);
    m_mine_detail->setFrameShape(QFrame::NoFrame);
    m_mine_shown = QStringLiteral("get");
    QTimer::singleShot(0, this, [this] { showGetPanel(); });
    split->addWidget(m_mine_detail);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    connect(m_mine, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const QString handle{item->data(Qt::UserRole).toString()};
        const QString reservation{item->data(Qt::UserRole + 1).toString()};
        m_mine_shown = handle.isEmpty() ? QStringLiteral("r:") + reservation : handle;
        if (handle.isEmpty()) {
            showReservation(reservation);
        } else {
            showDetail(m_mine_detail, handle, true);
        }
    });
    return split;
}

QWidget* BitNamesPage::createExploreTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    layout->setContentsMargins(0, 6, 0, 0);
    m_search = new QLineEdit(tab);
    m_search->setPlaceholderText(tr("Find a name, a subname (blog.core), a number (1739-0029) — or resolve a domain: www.core.x"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumHeight(28);
    layout->addWidget(m_search);
    m_resolved = new QLabel(tab);
    m_resolved->setWordWrap(true);
    m_resolved->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_resolved->hide();
    layout->addWidget(m_resolved);
    auto* split{new QSplitter(Qt::Horizontal, tab)};
    split->setChildrenCollapsible(false);
    m_all = PickList(split);
    split->addWidget(m_all);
    m_all_detail = new QScrollArea(split);
    m_all_detail->setWidgetResizable(true);
    m_all_detail->setFrameShape(QFrame::NoFrame);
    m_all_detail->setWidget(Dim(tr("Pick a name, or find one above."), m_all_detail));
    split->addWidget(m_all_detail);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);

    connect(m_all, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item) showDetail(m_all_detail, item->data(Qt::UserRole).toString(), false);
    });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& words) {
        for (int i{0}; i < m_all->count(); ++i) m_all->item(i)->setHidden(!words.isEmpty() && !m_all->item(i)->text().contains(words, Qt::CaseInsensitive));
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        const QString query{m_search->text().trimmed()};
        if (query.isEmpty()) return;
        // A domain under .x: what DNS answers for it.
        if (query.endsWith(QStringLiteral(".x"), Qt::CaseInsensitive) || query.endsWith(QStringLiteral(".x."), Qt::CaseInsensitive)) {
            const auto r{call("resolvename", Args({query.toStdString()}), false)};
            if (!r) return;
            QStringList lines{tr("<b>%1</b> — %2").arg(query.toHtmlEscaped(), Text((*r)["status"]))};
            for (const UniValue& record : (*r)["records"].getValues()) lines << QStringLiteral("%1 &nbsp; <tt>%2</tt>").arg(Text(record["type"]), Text(record["data"]).toHtmlEscaped());
            m_resolved->setText(lines.join(QStringLiteral("<br>")));
            m_resolved->show();
            return;
        }
        m_resolved->hide();
        if (const auto info{call("getname", Args({query.toStdString()}), false)}) showDetail(m_all_detail, QStringLiteral("0x") + Text((*info)["namehash"]), false);
    });
    return tab;
}

QWidget* BitNamesPage::createMessagesTab()
{
    auto* tab{new QWidget(this)};
    auto* layout{new QVBoxLayout(tab)};
    layout->addWidget(Heading(tr("Pay a name, with a message only its owner can read"), tab));
    auto* form{new QFormLayout};
    m_mail_to = new QLineEdit(tab);
    m_mail_to->setPlaceholderText(tr("The name"));
    form->addRow(tr("To"), m_mail_to);
    m_mail_amount = new QDoubleSpinBox(tab);
    m_mail_amount->setDecimals(8);
    m_mail_amount->setRange(0, 21000000);
    m_mail_amount->setValue(0.001);
    form->addRow(tr("Pay"), m_mail_amount);
    m_mail_text = new QPlainTextEdit(tab);
    m_mail_text->setMaximumHeight(80);
    form->addRow(tr("Message"), m_mail_text);
    layout->addLayout(form);
    auto* send{new QPushButton(tr("Send"), tab)};
    layout->addWidget(send, 0, Qt::AlignRight);
    layout->addWidget(Heading(tr("Received"), tab));
    m_inbox = SimpleTable({tr("Message"), tr("Paid"), tr("Confirmations")}, tab);
    m_inbox->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_inbox, 1);

    // Encrypting and signing by hand, folded away.
    auto* tools{new QWidget(tab)};
    auto* tools_layout{new QVBoxLayout(tools)};
    m_crypt_name = new QLineEdit(tools);
    m_crypt_name->setPlaceholderText(tr("A name: to encrypt to, to sign as (yours), or whose signature to check"));
    tools_layout->addWidget(m_crypt_name);
    m_crypt_in = new QPlainTextEdit(tools);
    m_crypt_in->setPlaceholderText(tr("Text, or an encrypted message"));
    m_crypt_in->setMaximumHeight(80);
    tools_layout->addWidget(m_crypt_in);
    auto* buttons{new QHBoxLayout};
    auto* encrypt{new QPushButton(tr("Encrypt"), tools)};
    auto* decrypt{new QPushButton(tr("Decrypt"), tools)};
    auto* sign{new QPushButton(tr("Sign"), tools)};
    auto* verify{new QPushButton(tr("Check a signature"), tools)};
    for (auto* b : {encrypt, decrypt, sign, verify}) buttons->addWidget(b);
    buttons->addStretch();
    tools_layout->addLayout(buttons);
    m_crypt_out = new QPlainTextEdit(tools);
    m_crypt_out->setReadOnly(true);
    m_crypt_out->setMaximumHeight(80);
    tools_layout->addWidget(m_crypt_out);
    layout->addWidget(Foldable(tr("Encrypt, decrypt, sign"), tools, tab));

    connect(send, &QPushButton::clicked, this, [this] {
        const QString text{m_mail_text->toPlainText()};
        if (m_mail_to->text().trimmed().isEmpty() || text.isEmpty()) return say(tr("Give a name and a message."), true);
        if (call("sendpaymail", Args({m_mail_to->text().trimmed().toStdString(), m_mail_amount->value(), text.toStdString()}), true)) {
            say(tr("Sent."));
            m_mail_text->clear();
        }
    });
    connect(encrypt, &QPushButton::clicked, this, [this] {
        if (const auto r{call("encryptnamemessage", Args({m_crypt_name->text().trimmed().toStdString(), m_crypt_in->toPlainText().toStdString()}), true)}) m_crypt_out->setPlainText(Text(*r));
    });
    connect(decrypt, &QPushButton::clicked, this, [this] {
        if (const auto r{call("decryptnamemessage", Args({m_crypt_in->toPlainText().trimmed().toStdString()}), true)}) m_crypt_out->setPlainText(Text(*r));
    });
    connect(sign, &QPushButton::clicked, this, [this] {
        if (const auto r{call("signasname", Args({m_crypt_name->text().trimmed().toStdString(), m_crypt_in->toPlainText().toStdString()}), true)}) m_crypt_out->setPlainText(Text(*r));
    });
    connect(verify, &QPushButton::clicked, this, [this] {
        m_busy = true;
        bool ok{false};
        const QString signature{QInputDialog::getText(this, tr("Check a signature"), tr("The signature:"), QLineEdit::Normal, {}, &ok).trimmed()};
        m_busy = false;
        if (!ok || signature.isEmpty()) return;
        if (const auto r{call("verifynamesignature", Args({m_crypt_name->text().trimmed().toStdString(), m_crypt_in->toPlainText().toStdString(), signature.toStdString()}), false)}) {
            m_crypt_out->setPlainText(r->isTrue() ? tr("Good: the owner of %1 signed this.").arg(m_crypt_name->text().trimmed()) : tr("Not a signature of this text by %1.").arg(m_crypt_name->text().trimmed()));
        }
    });
    return tab;
}

std::optional<UniValue> BitNamesPage::call(const std::string& method, const UniValue& params, bool wallet, bool quiet)
{
    if (!m_client_model) return std::nullopt;
    QString error;
    std::optional<QString> wallet_name;
    if (wallet) {
        wallet_name = m_wallet_name();
        if (!wallet_name) {
            if (!quiet) say(tr("Open or create a wallet first."), true);
            return std::nullopt;
        }
    }
    auto result{NodeRpc::Call(m_client_model, method, params, error, wallet_name)};
    if (!result && !quiet) say(error, true);
    return result;
}

void BitNamesPage::say(const QString& text, bool error)
{
    m_status->setStyleSheet(error ? QStringLiteral("color: %1;").arg(QLatin1String(RED)) : QString{});
    m_status->setText(text);
}

void BitNamesPage::refresh()
{
    if (!m_client_model || m_busy) return;
    if (const auto info{call("getbitnamesinfo", UniValue{UniValue::VARR}, false, true)}) {
        const bool dns{info->exists("dns")};
        m_dns_chip->setText(dns ? tr("● .x names resolve here") : tr("○ .x names do not resolve here"));
        m_dns_chip->setStyleSheet(QStringLiteral("color: %1; border: none;").arg(QLatin1String(dns ? GREEN : GREY)));
        m_dns_chip->setToolTip(dns ? tr("This node answers DNS for .x on %1. Click for how to use it.").arg(Text((*info)["dns"])) : tr("Click for how to resolve .x names on this computer."));
    }
    registerPending();
    if (m_tabs->currentIndex() == 0) refreshMine();
    if (m_tabs->currentIndex() == 1) refreshAll();
    if (m_tabs->currentIndex() == 2) refreshInbox();
}

void BitNamesPage::refreshMine()
{
    const auto mine{call("listmynames", UniValue{UniValue::VARR}, true, true)};
    if (!mine) return;
    // The list is redone only when it changed, and keeps what is picked; the panel is left alone.
    const std::string listed{mine->write()};
    if (listed == m_mine_listed) return;
    m_mine_listed = listed;
    const QSignalBlocker blocker{m_mine};
    m_mine->clear();
    QListWidgetItem* picked{nullptr};
    for (const UniValue& n : mine->getValues()) {
        const bool is_name{Text(n["kind"]) == QLatin1String("name")};
        const QString text{n.exists("name") ? Text(n["name"]) : (is_name ? tr("#%1").arg(Text(n["seq"])) : tr("(reservation)"))};
        auto* item{new QListWidgetItem(is_name ? text : tr("%1  — being registered").arg(text), m_mine)};
        const QString handle{is_name ? QStringLiteral("0x") + Text(n["namehash"]) : QString{}};
        item->setData(Qt::UserRole, handle);
        item->setData(Qt::UserRole + 1, is_name ? QString{} : (n.exists("name") ? Text(n["name"]) : QString{}));
        item->setData(Qt::UserRole + 2, text);
        item->setToolTip(tr("Held by %1").arg(Text(n["address"])));
        if (!is_name) item->setForeground(QColor{ORANGE});
        const QString key{handle.isEmpty() ? QStringLiteral("r:") + item->data(Qt::UserRole + 1).toString() : handle};
        if (key == m_mine_shown) picked = item;
    }
    if (m_mine->count() == 0) {
        auto* item{new QListWidgetItem(tr("No names yet: get one above."), m_mine)};
        item->setFlags(Qt::NoItemFlags);
    }
    if (picked) m_mine->setCurrentItem(picked);
}

void BitNamesPage::refreshAll()
{
    const auto names{call("listnames", Args({1000, 0}), false, true)};
    if (!names) return;
    const std::string listed{names->write()};
    if (listed == m_all_listed) return;
    m_all_listed = listed;
    const QString picked{m_all->currentItem() ? m_all->currentItem()->data(Qt::UserRole).toString() : QString{}};
    const QSignalBlocker blocker{m_all};
    m_all->clear();
    for (const UniValue& n : names->getValues()) {
        const QString handle{QStringLiteral("0x") + Text(n["namehash"])};
        auto* item{new QListWidgetItem(n.exists("name") ? QStringLiteral("%1   #%2").arg(Text(n["name"]), Text(n["seq"])) : tr("#%1   (private)").arg(Text(n["seq"])), m_all)};
        item->setData(Qt::UserRole, handle);
        if (handle == picked) m_all->setCurrentItem(item);
    }
    if (!m_search->text().isEmpty()) Q_EMIT m_search->textChanged(m_search->text());
}

void BitNamesPage::refreshInbox()
{
    const auto mail{call("listpaymail", UniValue{UniValue::VARR}, true, true)};
    if (!mail) return;
    m_inbox->setRowCount(0);
    for (const UniValue& m : mail->getValues()) {
        const int row{m_inbox->rowCount()};
        m_inbox->insertRow(row);
        m_inbox->setItem(row, 0, Item(m.exists("message") ? Text(m["message"]) : tr("(not for a key of this wallet)")));
        m_inbox->setItem(row, 1, Item(Text(m["amount"])));
        m_inbox->setItem(row, 2, Item(Text(m["confirmations"])));
    }
}

void BitNamesPage::getName(const QString& name, bool make_public)
{
    if (name.isEmpty()) return;
    const auto wallet{m_wallet_name()};
    if (!wallet) return say(tr("Open or create a wallet first."), true);
    // label.parent is a subname, made at once by the holder of the parent.
    if (name.contains(QLatin1Char('.'))) {
        const QString label{name.section(QLatin1Char('.'), 0, 0)};
        const QString parent{name.section(QLatin1Char('.'), 1)};
        if (call("createsubname", Args({parent.toStdString(), label.toStdString()}), true)) say(tr("%1 is yours after the next block.").arg(name));
        return;
    }
    if (call("reservename", Args({name.toStdString()}), true)) {
        QSettings settings;
        QStringList pending{settings.value(PendingKey(*wallet)).toStringList()};
        const QString entry{make_public ? name + QStringLiteral("\tpublic") : name};
        if (!pending.contains(entry)) pending << entry;
        settings.setValue(PendingKey(*wallet), pending);
        say(tr("%1 is reserved: it is registered by itself after the next block, and is yours after the one after.").arg(name));
        m_mine_listed.clear();
        refresh();
    }
}

void BitNamesPage::showGetPanel()
{
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(tr("Get a name"), panel)};
    title->setObjectName("nameTitle");
    layout->addWidget(title);
    layout->addWidget(Dim(tr("A name is yours for good: point it at your servers (name.x), hand out subnames, get paid with messages. "
                             "Type one to see if it is free; for a subname of a name of yours, type it under it, such as blog.yourname."), panel));
    auto* search{new QLineEdit(panel)};
    search->setObjectName("nameSearch");
    search->setPlaceholderText(tr("yourname"));
    search->setMinimumHeight(38);
    layout->addWidget(search);
    auto* availability{new QLabel(panel)};
    availability->setObjectName("availability");
    layout->addWidget(availability);
    auto* make_public{new QCheckBox(tr("Make it public"), panel)};
    layout->addWidget(make_public);
    layout->addWidget(Dim(tr("Public: the name's text is on the chain, so anyone can find it by name and see it listed. "
                             "Otherwise only its hash is: it works the same, but only those who know it can find it."), panel));
    auto* get{new QPushButton(tr("Get it"), panel)};
    get->setMinimumHeight(34);
    get->setEnabled(false);
    layout->addWidget(get, 0, Qt::AlignLeft);
    layout->addWidget(Heading(tr("How it works"), panel));
    layout->addWidget(Dim(tr("1. A reservation hides the name, so that nobody can see it coming and take it first. "
                             "2. After the next block, this page registers it. 3. After the block after that, it is yours. "
                             "Keep this page open meanwhile, or come back to it. A subname needs no reservation: it is yours after the next block."), panel));
    layout->addStretch();

    // As the name is typed: whether it can be had.
    auto* check{new QTimer(panel)};
    check->setSingleShot(true);
    check->setInterval(350);
    connect(search, &QLineEdit::textChanged, check, [check] { check->start(); });
    connect(check, &QTimer::timeout, panel, [this, search, availability, get, make_public] {
        const QString name{search->text().trimmed().toLower()};
        get->setEnabled(false);
        if (name.isEmpty()) return availability->clear();
        const auto colour{[&](const char* c) { availability->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(c))); }};
        if (name.contains(QLatin1Char('.'))) {
            make_public->setEnabled(false);
            const QString label{name.section(QLatin1Char('.'), 0, 0)};
            const QString parent{name.section(QLatin1Char('.'), 1)};
            if (call("getname", Args({name.toStdString()}), false, true)) {
                colour(RED);
                return availability->setText(tr("%1 exists already").arg(name));
            }
            bool mine{false};
            if (const auto names{call("listmynames", UniValue{UniValue::VARR}, true, true)}) {
                for (const UniValue& n : names->getValues()) mine |= Text(n["kind"]) == QLatin1String("name") && n.exists("name") && Text(n["name"]) == parent;
            }
            if (!mine) {
                colour(RED);
                return availability->setText(tr("Only the holder of %1 can make %2").arg(parent, name));
            }
            colour(GREEN);
            availability->setText(tr("✓ %1 can be yours: a subname of your %2").arg(name, parent));
            get->setEnabled(true);
            return;
        }
        make_public->setEnabled(true);
        if (const auto info{call("getname", Args({name.toStdString()}), false, true)}) {
            colour(RED);
            return availability->setText(tr("%1 is taken (#%2)").arg(name, Text((*info)["seq"])));
        }
        colour(GREEN);
        availability->setText(tr("✓ %1 is free").arg(name));
        get->setEnabled(true);
    });
    const auto take{[this, search, make_public, get] {
        if (!get->isEnabled()) return;
        getName(search->text().trimmed().toLower(), make_public->isChecked());
        search->clear();
    }};
    connect(get, &QPushButton::clicked, this, take);
    connect(search, &QLineEdit::returnPressed, this, take);

    if (QWidget* old{m_mine_detail->takeWidget()}) old->deleteLater();
    m_mine_detail->setWidget(panel);
    search->setFocus();
}

void BitNamesPage::registerPending()
{
    const auto wallet{m_wallet_name()};
    if (!wallet) return;
    QSettings settings;
    QStringList pending{settings.value(PendingKey(*wallet)).toStringList()};
    if (pending.isEmpty()) return;
    const auto mine{call("listmynames", UniValue{UniValue::VARR}, true, true)};
    if (!mine) return;
    QStringList left;
    for (const QString& entry : pending) {
        const QString name{entry.section(QLatin1Char('\t'), 0, 0)};
        const bool make_public{entry.endsWith(QStringLiteral("\tpublic"))};
        bool reserved{false}, registered{false};
        for (const UniValue& n : mine->getValues()) {
            if (!n.exists("name") || Text(n["name"]) != name) continue;
            if (Text(n["kind"]) == QLatin1String("reservation")) reserved = true;
            if (Text(n["kind"]) == QLatin1String("name")) registered = true;
        }
        if (registered) continue;
        if (reserved && call("registername", Args({name.toStdString(), UniValue{UniValue::VOBJ}, make_public}), true, true)) {
            say(tr("%1 is being registered.").arg(name));
        }
        left << entry;
    }
    settings.setValue(PendingKey(*wallet), left);
}

void BitNamesPage::showReservation(const QString& name)
{
    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};
    auto* title{new QLabel(name.isEmpty() ? tr("A reservation") : name, panel)};
    title->setObjectName("nameTitle");
    layout->addWidget(title);
    layout->addWidget(Dim(tr("Reserved: it is registered by itself once the reservation is in a block. A reservation for a name that is taken "
                             "can be released."), panel));
    auto* release{new QPushButton(tr("Release this reservation"), panel)};
    release->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(RED)));
    layout->addWidget(release, 0, Qt::AlignLeft);
    layout->addStretch();
    connect(release, &QPushButton::clicked, this, [this, name] {
        if (name.isEmpty()) return say(tr("This reservation was not made with this page: use releasereservation <name>."), true);
        m_busy = true;
        const bool yes{QMessageBox::question(this, tr("Release"), tr("Give up the reservation of %1?").arg(name)) == QMessageBox::Yes};
        m_busy = false;
        if (yes && call("releasereservation", Args({name.toStdString()}), true)) say(tr("The reservation of %1 is released after the next block.").arg(name));
    });
    // The panel shown before goes once control is back in the event loop: what called this may be in it.
    if (QWidget* old{m_mine_detail->takeWidget()}) old->deleteLater();
    m_mine_detail->setWidget(panel);
}

void BitNamesPage::showDetail(QScrollArea* area, const QString& handle, bool editable)
{
    const auto info{call("getname", Args({handle.toStdString()}), false)};
    if (!info) return;
    const UniValue data{info->exists("data") ? (*info)["data"] : UniValue{UniValue::VOBJ}};
    QString shown{info->exists("name") ? Text((*info)["name"]) : QString{}};
    if (shown.isEmpty() && editable && m_mine->currentItem()) shown = m_mine->currentItem()->data(Qt::UserRole + 2).toString();
    if (shown.isEmpty()) shown = QStringLiteral("#") + Text((*info)["seq"]);

    auto* panel{new QWidget};
    auto* layout{new QVBoxLayout(panel)};

    // What it is.
    auto* title{new QLabel(shown.startsWith(QLatin1Char('#')) ? shown : shown + QStringLiteral(".x"), panel)};
    title->setObjectName("nameTitle");
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    auto* tags{new QHBoxLayout};
    const bool subname{info->exists("parent")};
    tags->addWidget(Pill(subname ? tr("SUBNAME") : tr("NAME"), subname ? PURPLE : GREEN, panel));
    tags->addWidget(Pill(QStringLiteral("#") + Text((*info)["seq"]), GREY, panel));
    if (info->exists("icann")) tags->addWidget(Pill(tr("ICANN"), ORANGE, panel));
    auto* owner{Dim(tr("held by %1").arg((*info)["owner"].exists("address") ? Text((*info)["owner"]["address"]) : tr("?")), panel)};
    owner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tags->addWidget(owner, 1);
    layout->addLayout(tags);

    // Where it points: the DNS records, to edit in place.
    layout->addWidget(Heading(tr("Where it points"), panel));
    layout->addWidget(Dim(editable ? tr("Host @ is %1.x itself; www is www.%1.x; * is anything under it. A value without a final dot is under %1.x.").arg(shown)
                                   : tr("What %1.x and the hosts under it answer.").arg(shown), panel));
    auto* records{SimpleTable({tr("Host"), tr("Type"), tr("Value"), tr("Priority")}, panel)};
    records->setMinimumHeight(160);
    records->horizontalHeader()->setStretchLastSection(false);
    records->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    records->setColumnWidth(0, 110);
    records->setColumnWidth(1, 110);
    records->setColumnWidth(3, 70);
    if (!editable) records->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (const auto rows{call("getnamerecords", Args({handle.toStdString()}), false, true)}) {
        for (const UniValue& r : rows->getValues()) {
            AddRecordRow(records, Text(r["host"]), Text(r["type"]), Text(r["value"]), r.exists("priority") ? Text(r["priority"]) : QString{}, editable);
        }
    }
    // Without records, the address of the name answers: show it, so that there is one place to look.
    if (records->rowCount() == 0 && data.exists("ipv4")) AddRecordRow(records, QStringLiteral("@"), QStringLiteral("A"), Text(data["ipv4"]).section(QLatin1Char(':'), 0, 0), {}, editable);
    layout->addWidget(records);
    if (editable) {
        auto* row{new QHBoxLayout};
        auto* add{new QPushButton(tr("Add a record"), panel)};
        auto* remove{new QPushButton(tr("Remove"), panel)};
        auto* save{new QPushButton(tr("Save"), panel)};
        row->addWidget(add);
        row->addWidget(remove);
        row->addStretch();
        row->addWidget(save);
        layout->addLayout(row);
        connect(add, &QPushButton::clicked, records, [records] {
            AddRecordRow(records, records->rowCount() == 0 ? QStringLiteral("@") : QString{}, QStringLiteral("A"), {}, {}, true);
            records->setCurrentCell(records->rowCount() - 1, 2);
            records->editItem(records->item(records->rowCount() - 1, 2));
        });
        connect(remove, &QPushButton::clicked, records, [records] {
            if (records->currentRow() >= 0) records->removeRow(records->currentRow());
        });
        connect(save, &QPushButton::clicked, this, [this, records, handle, shown] {
            UniValue rows(UniValue::VARR);
            for (int r{0}; r < records->rowCount(); ++r) {
                const auto cell{[&](int c) { return records->item(r, c) ? records->item(r, c)->text().trimmed() : QString{}; }};
                if (cell(2).isEmpty()) continue;
                UniValue row(UniValue::VOBJ);
                row.pushKV("host", cell(0).isEmpty() ? std::string{"@"} : cell(0).toStdString());
                row.pushKV("type", static_cast<QComboBox*>(records->cellWidget(r, 1))->currentText().toStdString());
                row.pushKV("value", cell(2).toStdString());
                bool ok{false};
                const int priority{cell(3).toInt(&ok)};
                if (ok) row.pushKV("priority", priority);
                rows.push_back(std::move(row));
            }
            if (call("setnamerecords", Args({handle.toStdString(), rows}), true)) say(tr("Saved: %1.x points there after the next block.").arg(shown));
        });
    }

    // The subnames under it.
    std::vector<std::pair<QString, QString>> subnames;
    if (const auto all{call("listnames", Args({1000, 0}), false, true)}) {
        for (const UniValue& n : all->getValues()) {
            if (n.exists("parent") && QStringLiteral("0x") + Text(n["parent"]) == handle) {
                subnames.emplace_back(Text(n["label"]), n["owner"].exists("address") ? Text(n["owner"]["address"]) : QString{});
            }
        }
    }
    if (editable || !subnames.empty()) layout->addWidget(Heading(tr("Subnames"), panel));
    for (const auto& [label, holder] : subnames) {
        auto* line{new QLabel(QStringLiteral("<b>%1.%2.x</b> &nbsp; <span style='color:%3'>held by %4</span>").arg(label.toHtmlEscaped(), shown.toHtmlEscaped(), QLatin1String(GREY), holder), panel)};
        line->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(line);
    }
    if (editable) {
        auto* row{new QHBoxLayout};
        auto* label{new QLineEdit(panel)};
        label->setPlaceholderText(tr("label, as blog for blog.%1").arg(shown));
        auto* holder{new QLineEdit(panel)};
        holder->setPlaceholderText(tr("held by: this wallet, or an address"));
        auto* create{new QPushButton(tr("Create"), panel)};
        row->addWidget(label, 1);
        row->addWidget(holder, 2);
        row->addWidget(create);
        layout->addLayout(row);
        connect(create, &QPushButton::clicked, this, [this, label, holder, handle, shown] {
            const QString text{label->text().trimmed().toLower()};
            if (text.isEmpty()) return;
            if (call("createsubname", Args({handle.toStdString(), text.toStdString(), holder->text().trimmed().toStdString()}), true)) {
                say(tr("%1.%2 exists after the next block.").arg(text, shown));
                label->clear();
                holder->clear();
            }
        });
    }

    if (editable) {
        // Everything else, folded away: keys and paymail, giving it away, releasing it.
        auto* more{new QWidget};
        auto* more_layout{new QVBoxLayout(more)};
        auto* form{new QFormLayout};
        auto* encryption{new QLineEdit(Field(data, "encryptionkey"), more)};
        auto* signing{new QLineEdit(Field(data, "signingkey"), more)};
        auto* paymail{new QLineEdit(Field(data, "paymailfee"), more)};
        paymail->setPlaceholderText(tr("none: no paid messages"));
        auto* commitment{new QLineEdit(Field(data, "commitment"), more)};
        form->addRow(tr("Messages are encrypted to"), encryption);
        form->addRow(tr("Messages are signed with"), signing);
        form->addRow(tr("Paid messages pay at least"), paymail);
        form->addRow(tr("Commitment (hash)"), commitment);
        more_layout->addLayout(form);
        auto* key_row{new QHBoxLayout};
        auto* new_keys{new QPushButton(tr("Use new keys of this wallet"), more)};
        auto* save_more{new QPushButton(tr("Save"), more)};
        key_row->addWidget(new_keys);
        key_row->addStretch();
        key_row->addWidget(save_more);
        more_layout->addLayout(key_row);
        auto* give_row{new QHBoxLayout};
        auto* give_to{new QLineEdit(more)};
        give_to->setPlaceholderText(tr("Give %1 to an address").arg(shown));
        auto* give{new QPushButton(tr("Give"), more)};
        give_row->addWidget(give_to, 1);
        give_row->addWidget(give);
        more_layout->addLayout(give_row);
        auto* release{new QPushButton(tr("Release %1 for good").arg(shown), more)};
        release->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(RED)));
        more_layout->addWidget(release, 0, Qt::AlignLeft);
        layout->addWidget(Foldable(tr("Keys, paid messages, give away, release"), more, panel));

        connect(new_keys, &QPushButton::clicked, this, [this, encryption, signing] {
            if (const auto k{call("getnewnamekeys", UniValue{UniValue::VARR}, true)}) {
                encryption->setText(Text((*k)["encryptionkey"]));
                signing->setText(Text((*k)["signingkey"]));
            }
        });
        connect(save_more, &QPushButton::clicked, this, [this, data, encryption, signing, paymail, commitment, handle, shown] {
            UniValue updates(UniValue::VOBJ);
            const auto put{[&](const char* key, QLineEdit* edit) {
                const QString now{edit->text().trimmed()};
                if (now == Field(data, key)) return;
                if (now.isEmpty()) {
                    updates.pushKV(key, UniValue{});
                } else if (std::string{key} == "paymailfee") {
                    UniValue amount;
                    amount.setNumStr(now.toStdString());
                    updates.pushKV(key, amount);
                } else {
                    updates.pushKV(key, now.toStdString());
                }
            }};
            put("encryptionkey", encryption);
            put("signingkey", signing);
            put("paymailfee", paymail);
            put("commitment", commitment);
            if (updates.empty()) return say(tr("Nothing changed."));
            if (call("updatename", Args({handle.toStdString(), updates}), true)) say(tr("Saved: %1 changes after the next block.").arg(shown));
        });
        connect(give, &QPushButton::clicked, this, [this, give_to, handle, shown] {
            const QString address{give_to->text().trimmed()};
            if (address.isEmpty()) return;
            m_busy = true;
            const bool yes{QMessageBox::question(this, tr("Give %1").arg(shown), tr("Give %1 to %2? Only the new holder can give it back.").arg(shown, address)) == QMessageBox::Yes};
            m_busy = false;
            if (yes && call("transfername", Args({handle.toStdString(), address.toStdString()}), true)) say(tr("%1 goes to %2 after the next block.").arg(shown, address));
        });
        connect(release, &QPushButton::clicked, this, [this, handle, shown] {
            m_busy = true;
            const bool yes{QMessageBox::warning(this, tr("Release %1").arg(shown), tr("Give up %1 for good? Its records go with it, and anyone can register it. This cannot be undone.").arg(shown),
                                                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes};
            m_busy = false;
            if (yes && call("releasename", Args({handle.toStdString(), true}), true)) say(tr("%1 is released after the next block.").arg(shown));
        });
    } else {
        // History, for names looked at.
        auto* history{SimpleTable({tr("Field"), tr("Value"), tr("Height")}, nullptr)};
        history->setEditTriggers(QAbstractItemView::NoEditTriggers);
        history->setMinimumHeight(140);
        if (const auto h{call("getnamehistory", Args({handle.toStdString()}), false, true)}) {
            for (const std::string& field : h->getKeys()) {
                for (const UniValue& entry : (*h)[field].getValues()) {
                    const int row{history->rowCount()};
                    history->insertRow(row);
                    history->setItem(row, 0, Item(QString::fromStdString(field)));
                    history->setItem(row, 1, Item(entry["value"].isNull() ? tr("(none)") : Show(entry["value"])));
                    auto* height{new QTableWidgetItem};
                    height->setData(Qt::DisplayRole, entry["height"].getInt<int>());
                    height->setFlags(height->flags() & ~Qt::ItemIsEditable);
                    history->setItem(row, 2, height);
                }
            }
            history->sortItems(2, Qt::DescendingOrder);
        }
        layout->addWidget(Foldable(tr("History"), history, panel));
    }
    layout->addStretch();

    // The panel shown before goes once control is back in the event loop: what called this may be in it.
    if (QWidget* old{area->takeWidget()}) old->deleteLater();
    area->setWidget(panel);
}

void BitNamesPage::showDnsHelp()
{
    m_busy = true;
    QMessageBox::information(this, tr(".x names on this computer"),
        tr("<p>This program answers DNS questions for .x names when started with <tt>-dnsbind=127.0.0.1:5335</tt> "
           "(or with <tt>dnsbind=127.0.0.1:5335</tt> in its configuration file).</p>"
           "<p>Then send the computer's .x questions to it:</p>"
           "<p><b>Linux (systemd-resolved):</b> <tt>resolvectl dns lo 127.0.0.1:5335</tt> and <tt>resolvectl domain lo ~x</tt><br>"
           "<b>Linux (dnsmasq):</b> <tt>server=/x/127.0.0.1#5335</tt><br>"
           "<b>macOS:</b> a file <tt>/etc/resolver/x</tt> with <tt>nameserver 127.0.0.1</tt> and <tt>port 5335</tt><br>"
           "<b>Windows:</b> start with <tt>-dnsbind=127.0.0.1:53</tt>, then in PowerShell <tt>Add-DnsClientNrptRule -Namespace \".x\" -NameServers 127.0.0.1</tt></p>"
           "<p>To check a domain, type it in Explore, such as <tt>www.core.x</tt>.</p>"));
    m_busy = false;
}
