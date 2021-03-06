// Copyright (c) 2020 The NavCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef AGGREGATIONSESSION_H
#define AGGREGATIONSESSION_H

#include <blsct/ephemeralserver.h>
#include <blsct/key.h>
#include <blsct/transaction.h>
#include <net.h>
#include <random.h>
#include <util.h>
#include <utilstrencodings.h>
#include <utiltime.h>
#include <wallet/wallet.h>

#define DEFAULT_MIX_FEE 10000000
#define DEFAULT_MIN_OUTPUT_AMOUNT 10000000000
#define DEFAULT_MAX_MIX_FEE 100000000
#define DEFAULT_TX_MIXCOINS 10
#define DEFAULT_MIX true

class AggregationSesion
{
public:
    AggregationSesion(const CStateViewCache* inputs);

    bool Start();
    void Stop();

    static CAmount GetDefaultFee();
    static CAmount GetMaxFee();

    static bool IsKnown(const AggregationSesion& ms);

    bool GetState() const;

    void AnnounceHiddenService();

    bool AddCandidateTransaction(const std::vector<unsigned char>& v);

    bool SelectCandidates(CandidateTransaction& ret);

    uint256 GetHash() const
    {
        return SerializeHash(*this);
    }

    std::string GetHiddenService() const
    {
        return sHiddenService;
    }

    std::vector<CandidateTransaction> GetTransactionCandidates() const
    {
        return vTransactionCandidates;
    }

    bool Join() const;

    friend inline  bool operator==(const AggregationSesion& a, const AggregationSesion& b) { return a.GetHiddenService() == b.GetHiddenService(); }
    friend inline  bool operator<(const AggregationSesion& a, const AggregationSesion& b) { return a.GetHiddenService() < b.GetHiddenService(); }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(this->nVersion);
        READWRITE(sHiddenService);
    }

private:
    EphemeralServer *es;
    const CStateViewCache* inputs;
    std::string sHiddenService;
    int fState;
    bool lock;

    int nVersion;

    std::vector<CandidateTransaction> vTransactionCandidates;
};

#endif // AGGREGATIONSESSION_H
