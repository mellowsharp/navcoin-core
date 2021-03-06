// Copyright (c) 2020 The NavCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mixsession.h"

std::set<uint256> setKnownSessions;

MixSession::MixSession(const CCoinsViewCache* inputsIn) : inputs(inputsIn), fState(0)
{
}

bool MixSession::Start()
{
    if (fState && (es&&es->IsRunning()))
        return false;

    if (!inputs)
    {
        Stop();
        return false;
    }

    setTransactionCandidates.clear();

    lock = false;

    es = new EphemeralServer([=](std::string& s) -> void {
        lock = true;
        sHiddenService = s;
        if (!fState)
            AnnounceHiddenService();
        fState = !sHiddenService.empty();
    }, [=](std::vector<unsigned char>& v) -> void {
        AddCandidateTransaction(v);
    });

    while(!lock) {}

    return true;
}

bool MixSession::IsKnown(const MixSession& ms)
{
    return setKnownSessions.find(ms.GetHash()) != setKnownSessions.end();
}

void MixSession::Stop()
{
    if (es)
        es->Stop();
    fState = 0;
    setTransactionCandidates.clear();
    RemoveDandelionMixSessionEmbargo(*this);
}

bool MixSession::GetState() const
{
    return fState && (es&&es->IsRunning());
}

bool MixSession::AddCandidateTransaction(const std::vector<unsigned char>& v)
{
    if (!inputs)
        return false;

    CandidateTransaction tx;
    CDataStream ss(std::vector<unsigned char>(v.begin(), v.end()), 0, 0);
    try
    {
        ss >> tx;
    }
    catch(...)
    {
        return error("MixSession::%s: Wrong serialization of transaction candidate\n", __func__);
    }

    CValidationState state;

    std::vector<RangeproofEncodedData> blsctData;

    if(!VerifyBLSCT(tx.tx, bls::PrivateKey::FromBN(Scalar::Rand().bn), blsctData, *inputs, state, false, tx.fee))
    {
        return error("MixSession::%s: Failed validation of transaction candidate %s\n", __func__, state.GetRejectReason());
    }

    setTransactionCandidates.insert(tx);
    return true;
}

void MixSession::AnnounceHiddenService()
{
    if (!GetBoolArg("-dandelion", true))
    {
        Stop();
    }
    else
    {
        uint256 hash = GetHash();

        int64_t nCurrTime = GetTimeMicros();
        int64_t nEmbargo = 1000000*DANDELION_EMBARGO_MINIMUM+PoissonNextSend(nCurrTime, DANDELION_EMBARGO_AVG_ADD);
        InsertDandelionMixSessionEmbargo(*this,nEmbargo);

        if (vNodes.size() <= 1)
            RelayMixSession(*this);
        else if (!LocalDandelionDestinationPushMixSession(*this))
            Stop();
    }
}

bool MixSession::Join() const
{
    setKnownSessions.insert(this->GetHash());

    if (!pwalletMain)
        return false;

    {
        LOCK(pwalletMain->cs_wallet);
        if (*(pwalletMain->mixSession) == *this)
            return false;
    }

    if (!GetBoolArg("-blsctmix", DEFAULT_MIX))
        return false;

    LogPrint("mixsession","MixSession::%s: joining %s\n", __func__, GetHiddenService());

    std::vector<COutput> vAvailableCoins;

    pwalletMain->AvailablePrivateCoins(vAvailableCoins, true, nullptr);

    if (vAvailableCoins.size() == 0)
        return false;

    random_shuffle(vAvailableCoins.begin(), vAvailableCoins.end(), GetRandInt);

    const CWalletTx *prevcoin = vAvailableCoins[0].tx;
    int prevout = vAvailableCoins[0].i;

    CAmount nAddedFee = GetDefaultFee();

    std::vector<bls::PrependSignature> vBLSSignatures;
    Scalar gammaIns = prevcoin->vGammas[prevout];
    Scalar gammaOuts = 0;

    CMutableTransaction candidate;
    candidate.nVersion = TX_BLS_INPUT_FLAG | TX_BLS_CT_FLAG;

    bls::PrivateKey blindingKey = bls::PrivateKey::FromBN(Scalar::Rand().bn);
    blsctDoublePublicKey k;
    blsctPublicKey pk;
    blsctKey bk, v, s;

    std::vector<shared_ptr<CReserveBLSCTKey>> reserveBLSCTKey;
    shared_ptr<CReserveBLSCTKey> rk(new CReserveBLSCTKey(pwalletMain));
    reserveBLSCTKey.insert(reserveBLSCTKey.begin(), std::move(rk));

    reserveBLSCTKey[0]->GetReservedKey(pk);

    {
        LOCK(pwalletMain->cs_wallet);

        if (!pwalletMain->GetBLSCTKey(pk, bk))
        {
            return error("MixSession::%s: Could not get private key from blsct pool.\n",__func__);
        }

        blindingKey = bk.GetKey();

        if (!pwalletMain->GetBLSCTDoublePublicKey(k))
        {
            return error("MixSession::%s: BLSCT not supported in your wallet\n",__func__);
        }

        if (!(pwalletMain->GetBLSCTViewKey(v)  && pwalletMain->GetBLSCTSpendKey(s)))
        {
            return error("MixSession::%s: BLSCT keys not available\n", __func__);
        }
    }

    // Input
    candidate.vin.push_back(CTxIn(prevcoin->GetHash(), prevout, CScript(),
                              std::numeric_limits<unsigned int>::max()-1));
    bls::PublicKey secret1 = bls::BLS::DHKeyExchange(v.GetKey(), prevcoin->vout[prevout].GetBlindingKey());

    Scalar spendingKey =  Scalar(Point(secret1).Hash(0)) * Scalar(s.GetKey());
    bls::PrivateKey signingKey = bls::PrivateKey::FromBN(spendingKey.bn);

    CHashWriter ss(SER_GETHASH, 0);
    ss << candidate.vin[0];
    uint256 txInHash = ss.GetHash();

    bls::PrependSignature sig = signingKey.SignPrependPrehashed((unsigned char*)(&txInHash));
    vBLSSignatures.push_back(sig);

    // Output
    CTxOut newTxOut(0, CScript());

    std::string strFailReason;

    if (!CreateBLSCTOutput(blindingKey, newTxOut, k, prevcoin->vAmounts[prevout]+nAddedFee, "Mix Reward", gammaOuts, strFailReason, true, vBLSSignatures))
    {
        return error("MixSession::%s: Error creating BLSCT output: %s\n",__func__, strFailReason);
    }

    candidate.vout.push_back(newTxOut);

    // Balance Sig
    Scalar diff = gammaIns-gammaOuts;
    bls::PrivateKey balanceSigningKey = bls::PrivateKey::FromBN(diff.bn);

    candidate.vchBalanceSig = balanceSigningKey.Sign(balanceMsg, sizeof(balanceMsg)).Serialize();

    // Tx Sig
    candidate.vchTxSig = bls::PrependSignature::Aggregate(vBLSSignatures).Serialize();

    CValidationState state;

    std::vector<RangeproofEncodedData> blsctData;

    CandidateTransaction tx(candidate, nAddedFee);

    if(!VerifyBLSCT(candidate, v.GetKey(), blsctData, *inputs, state, false, nAddedFee))
        return error("MixSession::%s: Failed validation of transaction candidate %s\n", __func__, state.GetRejectReason());

    size_t colonPos = GetHiddenService().find(':');

    if(colonPos != std::string::npos)
    {
         std::string host = GetHiddenService().substr(0,colonPos);
         std::string portPart = GetHiddenService().substr(colonPos+1);

         std::stringstream parser(portPart);

         int port = 0;
         if( parser >> port )
         {
             SOCKET so = INVALID_SOCKET;
             bool fFailed;

             proxyType proxy;
             GetProxy(NET_TOR, proxy);

             // first connect to proxy server
             if (!ConnectSocketDirectly(proxy.proxy, so, 60)) {
                 return error("MixSession::%s: Failed to join session %s:%d, could not connect to tor socks proxy\n", __func__, host, port);
             }

             // do socks negotiation
             if (!Socks5(host, (unsigned short)port, 0, so))
                 return error("MixSession::%s: Failed to join session %s:%d, could not negotiate with tor socks proxy\n", __func__, host, port);

             if (!IsSelectableSocket(so)) {
                 CloseSocket(so);
                 return error("MixSession::%s: Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n", __func__);
             }

             CDataStream ds(0,0);
             ds << tx;
             std::vector<unsigned char> vBuffer(ds.begin(), ds.end());
             vBuffer.push_back(EPH_SERVER_DELIMITER);

             auto ret = send(so, vBuffer.data(), vBuffer.size(), MSG_NOSIGNAL);
             LogPrintf("MixSession::%s: Sent %d bytes\n", __func__, ret);

             CloseSocket(so);

             return ret;
         }

    }

    return false;
}

CAmount MixSession::GetDefaultFee()
{
    return GetArg("-mixinfee", DEFAULT_MIX_FEE);
}
