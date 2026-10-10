// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_BITNAMESTESTS_H
#define BITCOIN_QT_TEST_BITNAMESTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

class BitNamesTests : public QObject
{
public:
    explicit BitNamesTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void bitnamesTests();
};

#endif // BITCOIN_QT_TEST_BITNAMESTESTS_H
