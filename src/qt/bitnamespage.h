// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BITNAMESPAGE_H
#define BITCOIN_QT_BITNAMESPAGE_H

#include <qt/noderpc.h>

#include <QWidget>

class ClientModel;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTableWidget;
class QTabWidget;
class QShowEvent;
QT_END_NAMESPACE

/**
 * The names (BitNames), a page of the wallet. Your names in a list, and what is picked in a
 * panel next to it: its DNS records to edit in place, its subnames, and, folded away, its keys,
 * transfer and release. Explore shows any name the same way, read only. Messages pays the owner
 * of a name with a message, and encrypts and signs.
 */
class BitNamesPage : public QWidget
{
    Q_OBJECT

public:
    explicit BitNamesPage(NodeRpc::WalletNameFn wallet_name, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);

protected:
    void showEvent(QShowEvent* event) override;

private:
    QWidget* createMineTab();
    QWidget* createExploreTab();
    QWidget* createMessagesTab();

    std::optional<UniValue> call(const std::string& method, const UniValue& params, bool wallet, bool quiet = false);
    void say(const QString& text, bool error = false);

    /** Update the lists, and what goes on by itself (registering reserved names); never what is being edited. */
    void refresh();
    void refreshMine();
    void refreshAll();
    void refreshInbox();
    void registerPending();
    void getName(const QString& name, bool make_public);
    /** The panel to find a name and get it. */
    void showGetPanel();

    /** Show a name in a panel: `handle` as the commands take it ("0x" and its hash); editable if it is the wallet's. */
    void showDetail(QScrollArea* area, const QString& handle, bool editable);
    void showReservation(const QString& name);
    void showDnsHelp();

    NodeRpc::WalletNameFn m_wallet_name;
    ClientModel* m_client_model{nullptr};
    QTabWidget* m_tabs{nullptr};
    QLabel* m_status{nullptr};
    QPushButton* m_dns_chip{nullptr};
    //! Set while a question is asked: nothing is rebuilt meanwhile.
    bool m_busy{false};

    QListWidget* m_mine{nullptr};
    QScrollArea* m_mine_detail{nullptr};
    QString m_mine_shown;
    std::string m_mine_listed;

    QLineEdit* m_search{nullptr};
    QListWidget* m_all{nullptr};
    QScrollArea* m_all_detail{nullptr};
    QLabel* m_resolved{nullptr};
    std::string m_all_listed;

    QLineEdit* m_mail_to{nullptr};
    QDoubleSpinBox* m_mail_amount{nullptr};
    QPlainTextEdit* m_mail_text{nullptr};
    QTableWidget* m_inbox{nullptr};
    QLineEdit* m_crypt_name{nullptr};
    QPlainTextEdit* m_crypt_in{nullptr};
    QPlainTextEdit* m_crypt_out{nullptr};
};

#endif // BITCOIN_QT_BITNAMESPAGE_H
