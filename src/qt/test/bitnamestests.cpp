// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/bitnamestests.h>

#include <bitnames/state.h>
#include <chainparams.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <key_io.h>
#include <node/context.h>
#include <qt/bitcoingui.h>
#include <qt/bitnamespage.h>
#include <qt/clientmodel.h>
#include <qt/modaloverlay.h>
#include <qt/networkstyle.h>
#include <qt/noderpc.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletcontroller.h>
#include <qt/walletframe.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <rpc/server.h>
#include <script/descriptor.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <util/chaintype.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

#include <memory>
#include <set>

using wallet::AddWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::CWallet;
using wallet::RemoveWallet;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WALLET_FLAG_DESCRIPTORS;

namespace {

/**
 * Mine the transactions of the mempool, parents first, and give the block to the names as a sidechain
 * node would: the test chain is not a sidechain, and does not keep the names itself.
 */
void Mine(TestChain100Setup& test)
{
    std::vector<CTransactionRef> pool;
    for (const auto& info : test.m_node.mempool->infoAll()) pool.push_back(info.tx);
    std::set<Txid> in_pool, placed;
    for (const auto& tx : pool) in_pool.insert(tx->GetHash());
    std::vector<CMutableTransaction> ordered;
    while (ordered.size() < pool.size()) {
        for (const auto& tx : pool) {
            if (placed.contains(tx->GetHash())) continue;
            if (std::all_of(tx->vin.begin(), tx->vin.end(), [&](const CTxIn& in) { return !in_pool.contains(in.prevout.hash) || placed.contains(in.prevout.hash); })) {
                ordered.emplace_back(*tx);
                placed.insert(tx->GetHash());
            }
        }
    }
    const CBlock block{test.CreateAndProcessBlock(ordered, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()))};
    LOCK(::cs_main);
    Chainstate& chainstate{test.m_node.chainman->ActiveChainstate()};
    QCOMPARE(chainstate.m_chain.Tip()->GetBlockHash(), block.GetHash());
    bitnames::State names{chainstate.SideCache()};
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        std::string reason;
        QVERIFY2(names.ApplyTx(*block.vtx[i], chainstate.m_chain.Height(), Params().GetConsensus().sidechain.bitnames, reason), reason.c_str());
    }
}

/** Run `action`, answering every message box with `button` (or OK); the texts of the boxes. */
QStringList WithMessageBoxes(QMessageBox::StandardButton button, const std::function<void()>& action)
{
    QStringList texts;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [button, &texts] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* box{qobject_cast<QMessageBox*>(widget)};
            if (!box || !box->isVisible()) continue;
            texts << box->text();
            QAbstractButton* choice{box->button(button)};
            if (!choice) choice = box->button(QMessageBox::Ok);
            if (choice) choice->click();
        }
    });
    timer.start(20);
    action();
    timer.stop();
    return texts;
}

/** Run `action`, answering the text dialog it opens with `text`. */
void WithInput(const QString& text, const std::function<void()>& action)
{
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&text] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog{qobject_cast<QInputDialog*>(widget)};
            if (!dialog || !dialog->isVisible()) continue;
            dialog->setTextValue(text);
            dialog->accept();
        }
    });
    timer.start(20);
    action();
    timer.stop();
}

template <typename T>
T* WithText(QWidget* root, const QString& text)
{
    for (T* widget : root->findChildren<T*>()) {
        if (widget->text() == text) return widget;
    }
    return nullptr;
}

QLineEdit* WithPlaceholder(QWidget* root, const QString& start)
{
    for (auto* edit : root->findChildren<QLineEdit*>()) {
        if (edit->placeholderText().startsWith(start)) return edit;
    }
    return nullptr;
}

QPlainTextEdit* TextWithPlaceholder(QWidget* root, const QString& start)
{
    for (auto* edit : root->findChildren<QPlainTextEdit*>()) {
        if (edit->placeholderText().startsWith(start)) return edit;
    }
    return nullptr;
}

/** What the page says at the bottom. */
QString Status(BitNamesPage& page)
{
    for (auto* label : page.findChildren<QLabel*>(QString{}, Qt::FindDirectChildrenOnly)) {
        if (label->objectName().isEmpty()) return label->text();
    }
    return {};
}

/** Look again, as the page does when it is shown. */
void Refresh(BitNamesPage& page)
{
    page.hide();
    page.show();
    QApplication::processEvents();
}

QStringList Items(QListWidget* list)
{
    QStringList items;
    for (int i{0}; i < list->count(); ++i) items << list->item(i)->text();
    return items;
}

QListWidgetItem* ItemStarting(QListWidget* list, const QString& start)
{
    for (int i{0}; i < list->count(); ++i) {
        if (list->item(i)->text().startsWith(start)) return list->item(i);
    }
    return nullptr;
}

std::optional<UniValue> Rpc(ClientModel& client_model, const std::string& method, const UniValue& params, bool wallet)
{
    QString error;
    auto result{NodeRpc::Call(&client_model, method, params, error, wallet ? std::optional<QString>{QString{}} : std::nullopt)};
    if (!result) qWarning() << method.c_str() << error;
    return result;
}

void TestPage(TestChain100Setup& test, ClientModel& client_model, WalletModel& wallet_model, const PlatformStyle& platform_style)
{
    QSettings().remove("BitNames");
    BitNamesPage page([] { return std::optional<QString>{QString{}}; });
    page.resize(1100, 800);
    page.setClientModel(&client_model);
    page.show();
    QApplication::processEvents();
    auto* tabs{page.findChild<QTabWidget*>()};
    // My names: the list under the button to get one, and the panel beside it; Explore: the others.
    QWidget* mine_tab{page.findChild<QPushButton*>("getButton")->parentWidget()};
    QListWidget* mine{mine_tab->findChild<QListWidget*>()};
    QScrollArea* mine_detail{mine_tab->parentWidget()->findChild<QScrollArea*>(QString{}, Qt::FindDirectChildrenOnly)};
    QListWidget* all{nullptr};
    for (auto* list : page.findChildren<QListWidget*>("nameList")) {
        if (list != mine) all = list;
    }
    QScrollArea* all_detail{all->parentWidget()->findChild<QScrollArea*>(QString{}, Qt::FindDirectChildrenOnly)};
    QVERIFY(mine && mine_detail && all && all_detail && all_detail != mine_detail);
    QCOMPARE(Items(mine), QStringList{"No names yet: get one above."});
    // Without a DNS server on this node, the chip says so; it explains how to have one.
    auto* chip{WithText<QPushButton>(&page, "○ .x names do not resolve here")};
    QVERIFY(chip);
    const QStringList help{WithMessageBoxes(QMessageBox::Ok, [&] { chip->click(); })};
    QCOMPARE(help.size(), 1);
    QVERIFY(help.at(0).contains("-dnsbind=127.0.0.1:5335"));

    // Get a name: the panel says whether it can be had, as it is typed.
    QTRY_VERIFY(mine_detail->widget() && mine_detail->widget()->findChild<QLineEdit*>("nameSearch"));
    auto* search{mine_detail->widget()->findChild<QLineEdit*>("nameSearch")};
    auto* availability{mine_detail->widget()->findChild<QLabel*>("availability")};
    auto* get{WithText<QPushButton>(mine_detail->widget(), "Get it")};
    auto* make_public{WithText<QCheckBox>(mine_detail->widget(), "Make it public")};
    QVERIFY(get && make_public);
    search->setText("bad name");
    QTRY_COMPARE(availability->text(), QString("A name is lower-case letters, digits, - and _"));
    QVERIFY(!get->isEnabled());
    search->setText("x.nobody");
    QTRY_COMPARE(availability->text(), QString("Only the holder of nobody can make x.nobody"));
    QVERIFY(!get->isEnabled() && !make_public->isEnabled());
    search->setText("Core");
    QTRY_COMPARE(availability->text(), QString("✓ core is free"));
    QVERIFY(get->isEnabled() && make_public->isEnabled());
    make_public->setChecked(true);
    get->click();
    QVERIFY2(Status(page).startsWith("core is reserved"), qPrintable(Status(page)));
    QCOMPARE(QSettings().value("BitNames/pending/").toStringList(), QStringList{"core\tpublic"});

    // Once the reservation is in a block, the page registers the name by itself.
    Mine(test);
    Refresh(page);
    QCOMPARE(Status(page), QString("core is being registered."));
    Mine(test);
    Refresh(page);
    QTRY_VERIFY(ItemStarting(mine, "core"));
    QCOMPARE(QSettings().value("BitNames/pending/").toStringList(), QStringList{});
    QCOMPARE(ItemStarting(mine, "core")->text(), QString("core"));

    // Taken now.
    WithText<QPushButton>(&page, "+  Get a new name")->click();
    QApplication::processEvents();
    search = mine_detail->widget()->findChild<QLineEdit*>("nameSearch");
    availability = mine_detail->widget()->findChild<QLabel*>("availability");
    search->setText("core");
    QTRY_VERIFY2(availability->text().startsWith("core is taken (#"), qPrintable(availability->text()));
    // A subname of a name of this wallet, from the same panel.
    search->setText("shop.core");
    QTRY_COMPARE(availability->text(), QString("✓ shop.core can be yours: a subname of your core"));
    WithText<QPushButton>(mine_detail->widget(), "Get it")->click();
    QCOMPARE(Status(page), QString("shop.core is yours after the next block."));
    Mine(test);
    search = mine_detail->widget()->findChild<QLineEdit*>("nameSearch");
    availability = mine_detail->widget()->findChild<QLabel*>("availability");
    search->setText("shop.core");
    QTRY_COMPARE(availability->text(), QString("shop.core exists already"));

    // The name, picked: its records to edit in place, its subnames, and more.
    Refresh(page);
    QTRY_VERIFY(ItemStarting(mine, "shop.core"));
    mine->setCurrentItem(ItemStarting(mine, "core"));
    QWidget* panel{mine_detail->widget()};
    QCOMPARE(panel->findChild<QLabel*>("nameTitle")->text(), QString("core.x"));
    auto* records{panel->findChild<QTableWidget*>()};
    QCOMPARE(records->rowCount(), 0);
    WithText<QPushButton>(panel, "Add a record")->click();
    QCOMPARE(records->rowCount(), 1);
    QCOMPARE(records->item(0, 0)->text(), QString("@"));
    records->item(0, 2)->setText("192.0.2.7");
    WithText<QPushButton>(panel, "Add a record")->click();
    records->item(1, 0)->setText("www");
    static_cast<QComboBox*>(records->cellWidget(1, 1))->setCurrentText("CNAME");
    records->item(1, 2)->setText("@");
    WithText<QPushButton>(panel, "Add a record")->click();
    records->setCurrentCell(2, 2);
    WithText<QPushButton>(panel, "Remove")->click();
    QCOMPARE(records->rowCount(), 2);
    WithText<QPushButton>(panel, "Save")->click();
    QCOMPARE(Status(page), QString("Saved: core.x points there after the next block."));
    Mine(test);
    // A subname, held by this wallet.
    WithPlaceholder(panel, "label, as blog")->setText("Blog");
    WithText<QPushButton>(panel, "Create")->click();
    QCOMPARE(Status(page), QString("blog.core exists after the next block."));
    Mine(test);
    // Keys for messages, and a fee for paid ones.
    QWidget* more{WithText<QPushButton>(panel, "Use new keys of this wallet")->parentWidget()};
    const QList<QLineEdit*> fields{more->findChildren<QLineEdit*>()};
    QCOMPARE(fields.size(), 5);
    WithText<QPushButton>(more, "Save")->click();
    QCOMPARE(Status(page), QString("Nothing changed."));
    WithText<QPushButton>(more, "Use new keys of this wallet")->click();
    QVERIFY(fields.at(0)->text().size() == 66 && fields.at(1)->text().size() == 64);
    fields.at(2)->setText("0.001");
    WithText<QPushButton>(more, "Save")->click();
    QCOMPARE(Status(page), QString("Saved: core changes after the next block."));
    Mine(test);
    Refresh(page);
    mine->setCurrentItem(ItemStarting(mine, "shop.core"));
    mine->setCurrentItem(ItemStarting(mine, "core"));
    panel = mine_detail->widget();
    records = panel->findChild<QTableWidget*>();
    QCOMPARE(records->rowCount(), 2);
    QCOMPARE(records->item(0, 2)->text(), QString("192.0.2.7"));
    QCOMPARE(records->item(1, 0)->text(), QString("www"));
    QCOMPARE(static_cast<QComboBox*>(records->cellWidget(1, 1))->currentText(), QString("CNAME"));
    bool listed_blog{false};
    for (auto* label : panel->findChildren<QLabel*>()) listed_blog |= label->text().contains("blog.core.x");
    QVERIFY(listed_blog);
    more = WithText<QPushButton>(panel, "Use new keys of this wallet")->parentWidget();
    QCOMPARE(more->findChildren<QLineEdit*>().at(2)->text(), QString("0.00100000"));
    // An SRV record keeps its weight and port through a save of the page, which shows only its priority.
    {
        UniValue rows;
        QVERIFY(rows.read(R"([{"type": "A", "value": "192.0.2.7"}, {"host": "www", "type": "CNAME", "value": "@"},
                              {"host": "_sip._tcp", "type": "SRV", "value": "sip", "priority": 10, "weight": 5, "port": 5060}])"));
        QVERIFY(Rpc(client_model, "setnamerecords", NodeRpc::Args({"core", rows}), true));
        Mine(test);
        Refresh(page);
        mine->setCurrentItem(ItemStarting(mine, "shop.core"));
        mine->setCurrentItem(ItemStarting(mine, "core"));
        panel = mine_detail->widget();
        records = panel->findChild<QTableWidget*>();
        QCOMPARE(records->rowCount(), 3);
        WithText<QPushButton>(panel, "Add a record")->click();
        static_cast<QComboBox*>(records->cellWidget(3, 1))->setCurrentText("TXT");
        records->item(3, 2)->setText("hello");
        WithText<QPushButton>(panel, "Save")->click();
        QCOMPARE(Status(page), QString("Saved: core.x points there after the next block."));
        Mine(test);
        const auto saved{Rpc(client_model, "getnamerecords", NodeRpc::Args({"core"}), false)};
        QVERIFY(saved);
        QCOMPARE(saved->size(), size_t{4});
        bool srv{false};
        for (const UniValue& r : saved->getValues()) {
            if (r["type"].get_str() != "SRV") continue;
            srv = true;
            QCOMPARE(r["priority"].getInt<int>(), 10);
            QCOMPARE(r["weight"].getInt<int>(), 5);
            QCOMPARE(r["port"].getInt<int>(), 5060);
        }
        QVERIFY(srv);
    }
    // A subname picked: its own panel, which says what it depends on.
    mine->setCurrentItem(ItemStarting(mine, "shop.core"));
    QCOMPARE(mine_detail->widget()->findChild<QLabel*>("nameTitle")->text(), QString("shop.core.x"));
    bool depends{false};
    for (auto* label : mine_detail->widget()->findChildren<QLabel*>()) depends |= label->text().startsWith("A subname depends on its parent");
    QVERIFY(depends);
    // Released, with a question first.
    QWidget* shop_panel{mine_detail->widget()};
    const QStringList asked{WithMessageBoxes(QMessageBox::Yes, [&] { WithText<QPushButton>(shop_panel, "Release shop.core for good")->click(); })};
    QCOMPARE(asked.size(), 1);
    QVERIFY(asked.at(0).startsWith("Give up shop.core for good?"));
    QCOMPARE(Status(page), QString("shop.core is released after the next block."));
    Mine(test);
    Refresh(page);
    QTRY_VERIFY(!ItemStarting(mine, "shop.core"));

    // A reservation made elsewhere: shown as being registered, and released from its panel.
    QVERIFY(Rpc(client_model, "reservename", NodeRpc::Args({"spare"}), true));
    Mine(test);
    Refresh(page);
    QTRY_VERIFY(ItemStarting(mine, "spare"));
    QCOMPARE(ItemStarting(mine, "spare")->text(), QString("spare  — being registered"));
    mine->setCurrentItem(ItemStarting(mine, "spare"));
    QCOMPARE(mine_detail->widget()->findChild<QLabel*>("nameTitle")->text(), QString("spare"));
    QWidget* reservation_panel{mine_detail->widget()};
    QCOMPARE(WithMessageBoxes(QMessageBox::Yes, [&] { WithText<QPushButton>(reservation_panel, "Release this reservation")->click(); }).size(), 1);
    QCOMPARE(Status(page), QString("The reservation of spare is released after the next block."));
    Mine(test);
    Refresh(page);
    QTRY_VERIFY(!ItemStarting(mine, "spare"));

    // Messages: paid, encrypted, signed.
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    auto* send{WithText<QPushButton>(&page, "Send")};
    send->click();
    QCOMPARE(Status(page), QString("Give a name and a message."));
    WithPlaceholder(&page, "The name")->setText("core");
    page.findChild<QDoubleSpinBox*>()->setValue(0.0001);
    auto* mail_text{page.findChildren<QPlainTextEdit*>().at(0)};
    mail_text->setPlainText("hello core");
    send->click();
    QVERIFY2(Status(page).contains("paymail of 0.001 at least"), qPrintable(Status(page)));
    page.findChild<QDoubleSpinBox*>()->setValue(0.01);
    send->click();
    QCOMPARE(Status(page), QString("Sent."));
    QVERIFY(mail_text->toPlainText().isEmpty());
    Mine(test);
    tabs->setCurrentIndex(0);
    tabs->setCurrentIndex(2);
    QTableWidget* inbox{nullptr};
    for (auto* table : page.findChildren<QTableWidget*>()) {
        if (table->columnCount() == 3 && table->horizontalHeaderItem(2) && table->horizontalHeaderItem(2)->text() == "Confirmations") inbox = table;
    }
    QVERIFY(inbox);
    QCOMPARE(inbox->rowCount(), 1);
    QCOMPARE(inbox->item(0, 0)->text(), QString("hello core"));
    QCOMPARE(inbox->item(0, 1)->text(), QString("0.01000000"));
    auto* crypt_name{WithPlaceholder(&page, "A name: to encrypt to")};
    auto* crypt_in{TextWithPlaceholder(&page, "Text, or an encrypted message")};
    QPlainTextEdit* crypt_out{nullptr};
    for (auto* edit : page.findChildren<QPlainTextEdit*>()) {
        if (edit->isReadOnly()) crypt_out = edit;
    }
    QVERIFY(crypt_name && crypt_in && crypt_out);
    crypt_name->setText("core");
    crypt_in->setPlainText("a secret");
    WithText<QPushButton>(&page, "Encrypt")->click();
    const QString encrypted{crypt_out->toPlainText()};
    QVERIFY(encrypted.size() > 100);
    crypt_in->setPlainText(encrypted);
    WithText<QPushButton>(&page, "Decrypt")->click();
    QCOMPARE(crypt_out->toPlainText(), QString("a secret"));
    crypt_in->setPlainText("signed text");
    WithText<QPushButton>(&page, "Sign")->click();
    const QString signature{crypt_out->toPlainText()};
    QCOMPARE(signature.size(), 128);
    WithInput(signature, [&] { WithText<QPushButton>(&page, "Check a signature")->click(); });
    QCOMPARE(crypt_out->toPlainText(), QString("Good: the owner of core signed this."));
    crypt_in->setPlainText("other text");
    WithInput(signature, [&] { WithText<QPushButton>(&page, "Check a signature")->click(); });
    QCOMPARE(crypt_out->toPlainText(), QString("Not a signature of this text by core."));

    // Explore: every name, any name's panel with its history, and what DNS answers.
    tabs->setCurrentIndex(1);
    QApplication::processEvents();
    QTRY_VERIFY(ItemStarting(all, "core   #"));
    QVERIFY(ItemStarting(all, "blog.core   #"));
    auto* find{WithPlaceholder(&page, "Find a name")};
    find->setText("blog");
    QVERIFY(ItemStarting(all, "core   #")->isHidden());
    QVERIFY(!ItemStarting(all, "blog.core   #")->isHidden());
    find->setText("nobody");
    Q_EMIT find->returnPressed();
    QVERIFY2(Status(page).startsWith("No such name"), qPrintable(Status(page)));
    find->setText("core");
    Q_EMIT find->returnPressed();
    QWidget* explored{all_detail->widget()};
    QCOMPARE(explored->findChild<QLabel*>("nameTitle")->text(), QString("core.x"));
    QVERIFY(!WithText<QPushButton>(explored, "Save"));
    auto* history_toggle{WithText<QToolButton>(explored, "History")};
    QVERIFY(history_toggle);
    history_toggle->click();
    QTableWidget* history{nullptr};
    for (auto* table : explored->findChildren<QTableWidget*>()) {
        if (table->horizontalHeaderItem(0) && table->horizontalHeaderItem(0)->text() == "Field") history = table;
    }
    QVERIFY(history && history->isVisible());
    QVERIFY(history->rowCount() >= 7);
    all->setCurrentItem(ItemStarting(all, "blog.core   #"));
    QCOMPARE(all_detail->widget()->findChild<QLabel*>("nameTitle")->text(), QString("blog.core.x"));
    find->setText("www.core.x");
    Q_EMIT find->returnPressed();
    bool resolved{false};
    for (auto* label : page.findChildren<QLabel*>()) resolved |= label->isVisible() && label->text().contains("<b>www.core.x</b> — NOERROR") && label->text().contains("core.x.");
    QVERIFY(resolved);

    // Given away, with the keys of this wallet removed: no longer this wallet's.
    tabs->setCurrentIndex(0);
    Refresh(page);
    mine->setCurrentItem(ItemStarting(mine, "blog.core"));
    mine->setCurrentItem(ItemStarting(mine, "core"));
    panel = mine_detail->widget();
    CKey stranger;
    stranger.MakeNewKey(true);
    const std::string away{EncodeDestination(WitnessV0KeyHash{stranger.GetPubKey()})};
    WithPlaceholder(panel, "Give core to an address")->setText(QString::fromStdString(away));
    const QStringList give{WithMessageBoxes(QMessageBox::Yes, [&] { WithText<QPushButton>(panel, "Give")->click(); })};
    QCOMPARE(give.size(), 1);
    QCOMPARE(Status(page), QString::fromStdString("core goes to " + away + " after the next block."));
    Mine(test);
    Refresh(page);
    QTRY_VERIFY(!ItemStarting(mine, "core"));
    const auto core{Rpc(client_model, "getname", NodeRpc::Args({"core"}), false)};
    QVERIFY(core);
    QCOMPARE((*core)["owner"]["address"].get_str(), away);
    QVERIFY(!(*core)["data"].exists("encryptionkey"));

    // The page in a wallet view, and the window's Names action.
    WalletView view(&wallet_model, &platform_style, nullptr);
    view.setClientModel(&client_model);
    view.gotoNamesPage();
    QVERIFY(qobject_cast<BitNamesPage*>(view.currentWidget()));

    // Without a wallet: it says to open one.
    BitNamesPage no_wallet([] { return std::optional<QString>{}; });
    no_wallet.setClientModel(&client_model);
    no_wallet.show();
    QApplication::processEvents();
    QScrollArea* no_wallet_detail{no_wallet.findChild<QPushButton*>("getButton")->parentWidget()->parentWidget()->findChild<QScrollArea*>(QString{}, Qt::FindDirectChildrenOnly)};
    QTRY_VERIFY(no_wallet_detail->widget() && no_wallet_detail->widget()->findChild<QLineEdit*>("nameSearch"));
    no_wallet_detail->widget()->findChild<QLineEdit*>("nameSearch")->setText("free");
    QTRY_VERIFY(WithText<QPushButton>(no_wallet_detail->widget(), "Get it")->isEnabled());
    WithText<QPushButton>(no_wallet_detail->widget(), "Get it")->click();
    QCOMPARE(Status(no_wallet), QString("Open or create a wallet first."));
    no_wallet.findChild<QTabWidget*>()->setCurrentIndex(2);
    WithPlaceholder(&no_wallet, "A name: to encrypt to")->setText("core");
    WithText<QPushButton>(&no_wallet, "Encrypt")->click();
    QCOMPARE(Status(no_wallet), QString("Open or create a wallet first."));
    QSettings().remove("BitNames");
}

void TestWindow(interfaces::Node& node, ClientModel& client_model, WalletModel& wallet_model, const PlatformStyle& platform_style)
{
    // The Names action of the window shows the page of the wallet.
    WalletFrame frame(&platform_style, nullptr);
    frame.setClientModel(&client_model);
    auto* view{new WalletView(&wallet_model, &platform_style, &frame)};
    QVERIFY(frame.addView(view));
    frame.setCurrentWallet(&wallet_model);
    frame.gotoNamesPage();
    QVERIFY(qobject_cast<BitNamesPage*>(view->currentWidget()));

    const std::unique_ptr<const NetworkStyle> network_style{NetworkStyle::instantiate(ChainType::REGTEST)};
    // The window shows the wallets its controller has: the wallet of this test, already loaded.
    WalletController controller(client_model, &platform_style, nullptr);
    BitcoinGUI window(node, &platform_style, network_style.get());
    interfaces::BlockAndHeaderTipInfo tip_info;
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{node.context()->chainman->ActiveChain().Tip()};
        tip_info.block_height = tip_info.header_height = tip->nHeight;
        tip_info.block_time = tip_info.header_time = tip->GetBlockTime();
        tip_info.verification_progress = 1.0;
    }
    window.setClientModel(&client_model, &tip_info);
    window.setWalletController(&controller, /*show_loading_minimized=*/true);
    QAction* names{nullptr};
    for (QAction* action : window.findChildren<QAction*>()) {
        // The action of the window (the menus have entries of their own that stand for it).
        if (action->text() == "&Names" && action->isCheckable() && !names) names = action;
    }
    QVERIFY(names);
    // The test chain is old, so the window shows that it is catching up, which leaves the pages until
    // it is hidden, as the user does.
    window.findChild<ModalOverlay*>()->showHide(/*hide=*/true, /*userRequested=*/true);
    QTRY_VERIFY(names->isEnabled());
    names->trigger();
    QVERIFY(names->isChecked());
    bool shown{false};
    for (auto* view_in_window : window.findChildren<WalletView*>()) shown |= qobject_cast<BitNamesPage*>(view_in_window->currentWidget()) != nullptr;
    QVERIFY(shown);
    window.removeAllWallets();
    window.setClientModel(nullptr);
}

void TestBitNamesGui(interfaces::Node& node)
{
    TestChain100Setup test;
    // Coinbase outputs old enough to spend.
    for (int i = 0; i < 5; ++i) test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    auto wallet_loader = interfaces::MakeWalletLoader(*test.m_node.chain, *Assert(test.m_node.args));
    test.m_node.wallet_loader = wallet_loader.get();
    node.setContext(&test.m_node);
    wallet_loader->registerRpcs();
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();

    // A wallet holding the coinbase key of the test chain, with addresses of its own for names.
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
        FlatSigningProvider provider;
        std::string error;
        auto descs = Parse("combo(" + EncodeSecret(test.coinbaseKey) + ")", provider, error, /*require_checksum=*/false);
        QVERIFY(descs.size() == 1);
        WalletDescriptor w_desc(std::move(descs.at(0)), 0, 0, 1, 1);
        QVERIFY(wallet->AddWalletDescriptor(w_desc, provider, "", false));
        wallet->SetLastBlockProcessed(105, WITH_LOCK(node.context()->chainman->GetMutex(), return node.context()->chainman->ActiveChain().Tip()->GetBlockHash()));
    }
    {
        wallet::WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const auto result{wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false)};
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    wallet->SetBroadcastTransactions(true);
    wallet->m_min_fee = CFeeRate{10000};
    wallet->m_fallback_fee = CFeeRate{10000};
    WalletContext& context = *node.walletLoader().context();
    AddWallet(context, wallet);

    std::unique_ptr<const PlatformStyle> platform_style(PlatformStyle::instantiate("other"));
    OptionsModel options_model(node);
    bilingual_str error;
    QVERIFY(options_model.Init(error));
    ClientModel client_model(node, &options_model);
    WalletModel wallet_model(interfaces::MakeWallet(context, wallet), client_model, platform_style.get());

    TestPage(test, client_model, wallet_model, *platform_style);
    TestWindow(node, client_model, wallet_model, *platform_style);

    RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt);
}

} // namespace

void BitNamesTests::bitnamesTests()
{
    // Let the single-shot timers earlier tests leave behind fire while no message box is open (see
    // SidechainTests).
    QTest::qWait(500);
    TestBitNamesGui(m_node);
}
