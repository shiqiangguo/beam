// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "negotiator.h"
#include "core/block_crypt.h"

namespace beam { namespace wallet
{
    using namespace ECC;
    using namespace std;

    BaseTransaction::BaseTransaction(INegotiatorGateway& gateway
                                   , beam::IKeyChain::Ptr keychain
                                   , const TxDescription& txDesc)
        : m_gateway{ gateway }
        , m_keychain{ keychain }
        , m_txDesc{ txDesc }
    {
        assert(keychain);
    }

    //void Negotiator::saveState()
    //{
    //    if (m_txDesc.canResume())
    //    {
    //        Serializer ser;
    //        ser & *this;
    //        ser.swap_buf(m_txDesc.m_fsmState);
    //    }
    //    else
    //    {
    //        m_txDesc.m_fsmState.clear();
    //    }
    //    m_keychain->saveTx(m_txDesc);
    //}

    TxKernel* BaseTransaction::getKernel() const
    {
        if (m_txDesc.m_status == TxDescription::Registered)
        {
            // TODO: what should we do in case when we have more than one kernel
            if (m_kernel)
            {
                return m_kernel.get();
            }
            else if (m_transaction && !m_transaction->m_vKernelsOutput.empty())
            {
                return m_transaction->m_vKernelsOutput[0].get();
            }
        }
        return nullptr;
    }

    const TxID& BaseTransaction::getTxID() const
    {
        return m_txDesc.m_txId;
    }

    void BaseTransaction::cancel()
    {
        if (m_txDesc.m_status == TxDescription::Pending)
        {
            m_keychain->deleteTx(m_txDesc.m_txId);
        }
        else
        {
            updateTxDescription(TxDescription::Cancelled);
            rollbackTx();
            m_gateway.send_tx_failed(m_txDesc);
        }
    }

    SendTransaction::SendTransaction(INegotiatorGateway& gateway
                                    , beam::IKeyChain::Ptr keychain
                                    , const TxDescription& txDesc)
        : BaseTransaction{ gateway, keychain, txDesc }
    {

    }

    void SendTransaction::update()
    {
        if (getTxInputs(m_txDesc.m_txId).empty())
        {
            LOG_INFO() << m_txDesc.m_txId << " Sending " << PrintableAmount(m_txDesc.m_amount) << " (fee: " << PrintableAmount(m_txDesc.m_fee) << ")";
            Height currentHeight = m_keychain->getCurrentHeight();

            createKernel(m_txDesc.m_fee, currentHeight);
            m_txDesc.m_minHeight = currentHeight;

            if (!prepareSenderUtxos(currentHeight))
            {
                onFailed();
                return;
            }

            updateTxDescription(TxDescription::InProgress);
        }

        auto address = m_keychain->getAddress(m_txDesc.m_peerId);

        if (address.is_initialized() && address->m_own)
        {
            sendSelfTx();
            return;
        }

        Scalar peerSignature;
        Point publicPeerNonce, publicPeerExcess;
        if (!getParameter(TxParams::PeerSignature, peerSignature)
            || !getParameter(TxParams::PublicPeerNonce, publicPeerNonce)
            || !getParameter(TxParams::PublicPeerExcess, publicPeerExcess))
        {
            sendInvite();
            return;
        }

        if (!isValidSignature(peerSignature, publicPeerNonce, publicPeerExcess)
            || !m_publicPeerExcess.Import(publicPeerExcess)
            || !m_publicPeerNonce.Import(publicPeerNonce))
        {
            onFailed(true);
            return;
        }

        bool isRegistered = false;
        if (!getParameter(TxParams::TransactionRegistered, isRegistered))
        {
            Point::Native pt = Context::get().G * m_blindingExcess;
            pt += m_publicPeerExcess;
            m_kernel->m_Excess = pt;

            m_peerSignature = peerSignature;
            updateTxDescription(TxDescription::InProgress);
            auto s = createSignature();
            Scalar::Native sn = s;
            m_kernel->m_Signature.m_k = sn + m_peerSignature;
            sendConfirmTransaction(s);
            return;
        }
        
        bool isConfirmed = false;
        if (!getParameter(TxParams::TransactionConfirmed, isConfirmed))
        {
            confirmOutputs();
            return;
        }

        completeTx();
    }

    void SendTransaction::invitePeer()
    {
        bool sender = m_txDesc.m_sender;
        LOG_INFO() << m_txDesc.m_txId << (sender ? " Sending " : " Receiving ") << PrintableAmount(m_txDesc.m_amount) << " (fee: " << PrintableAmount(m_txDesc.m_fee) << ")";


        Height currentHeight = m_keychain->getCurrentHeight();

        createKernel(m_txDesc.m_fee, currentHeight);
        m_txDesc.m_minHeight = currentHeight;

        if (sender)
        {
            if (!prepareSenderUtxos(currentHeight))
            {
                onFailed();
                return;
            }
        }
        else
        {
            createOutputUtxo(m_txDesc.m_amount, currentHeight);
        }

        updateTxDescription(TxDescription::InProgress);

        if (sender)
        {
            auto address = m_keychain->getAddress(m_txDesc.m_peerId);

            if (address.is_initialized() && address->m_own)
            {
                sendSelfTx();
                return;
            }
        }

        sendInvite();
    }

    void SendTransaction::sendSelfTx()
    {
        // Create output UTXOs for main amount
        createOutputUtxo(m_txDesc.m_amount, m_txDesc.m_minHeight);

        // Create empty transaction
        m_transaction = std::make_shared<Transaction>();
        m_transaction->m_Offset = Zero;

        // Calculate public key for excess
        Point::Native excess;
        if (!excess.Import(getPublicExcess()))
        {
            //onFailed(true);
            return;
        }

        // Calculate signature
        Scalar::Native signature = createSignature();

        // Construct and verify transaction
        if (!constructTxInternal(signature))
        {
            //onFailed(true);
            return;
        }

        updateTxDescription(TxDescription::InProgress);
        sendNewTransaction();
    }

    void SendTransaction::sendInvite() const
    {
        bool sender = m_txDesc.m_sender;
        Height currentHeight = m_txDesc.m_minHeight;
        const TxID& txID = m_txDesc.m_txId;

        Invite inviteMsg;
        inviteMsg.m_txId = txID;
        inviteMsg.m_amount = m_txDesc.m_amount;
        inviteMsg.m_fee = m_txDesc.m_fee;
        inviteMsg.m_height = currentHeight;
        inviteMsg.m_send = sender;
        inviteMsg.m_inputs = getTxInputs(txID);
        inviteMsg.m_outputs = getTxOutputs(txID);
        inviteMsg.m_publicPeerExcess = getPublicExcess();
        inviteMsg.m_publicPeerNonce = getPublicNonce();
        inviteMsg.m_offset = m_offset;

        m_gateway.send_tx_invitation(m_txDesc, move(inviteMsg));
    }

    bool SendTransaction::confirmPeer()
    {
        Scalar peerSignature;
        Point publicPeerNonce, publicPeerExcess;
        if (!getParameter(TxParams::PeerSignature, peerSignature)
         || !getParameter(TxParams::PublicPeerNonce, publicPeerNonce)
         || !getParameter(TxParams::PublicPeerExcess, publicPeerExcess))
        {
            return false;
        }

        if (!isValidSignature(peerSignature, publicPeerNonce, publicPeerExcess)
            || !m_publicPeerExcess.Import(publicPeerExcess)
            || !m_publicPeerNonce.Import(publicPeerNonce))
        {
            onFailed(true);
            return false;
        }

        Point::Native pt = Context::get().G * m_blindingExcess;
        pt += m_publicPeerExcess;
        m_kernel->m_Excess = pt;

        m_peerSignature = peerSignature;
        updateTxDescription(TxDescription::InProgress);
        auto s = createSignature();
        Scalar::Native sn = s;
        m_kernel->m_Signature.m_k = sn + m_peerSignature;
        sendConfirmTransaction(s);
        return true;
    }

    void SendTransaction::sendConfirmTransaction(const Scalar& peerSignature) const
    {
        ConfirmTransaction confirmMsg;
        confirmMsg.m_txId = m_txDesc.m_txId;
        confirmMsg.m_peerSignature = peerSignature;

        m_gateway.send_tx_confirmation(m_txDesc, move(confirmMsg));
    }

    ReceiveTransaction::ReceiveTransaction(INegotiatorGateway& gateway
                                         , beam::IKeyChain::Ptr keychain
                                         , const TxDescription& txDesc)
        : BaseTransaction{ gateway, keychain, txDesc }
    {

    }


    void ReceiveTransaction::update()
    {
        if (getTxOutputs(m_txDesc.m_txId).empty())
        {
            LOG_INFO() << m_txDesc.m_txId << " Receiving " << PrintableAmount(m_txDesc.m_amount) << " (fee: " << PrintableAmount(m_txDesc.m_fee) << ")";

            Scalar offset;
            Point publicPeerNonce, publicPeerExcess;
            if (!getParameter(TxParams::PeerOffset, offset)
                || !getParameter(TxParams::PublicPeerNonce, publicPeerNonce)
                || !getParameter(TxParams::PublicPeerExcess, publicPeerExcess)
                || m_offset.Import(offset)
                || !m_publicPeerNonce.Import(publicPeerNonce)
                || !m_publicPeerExcess.Import(publicPeerExcess))
            {
                onFailed(true);
                return;
            }

            Height currentHeight = m_keychain->getCurrentHeight();
            createKernel(m_txDesc.m_fee, m_txDesc.m_minHeight);
            createOutputUtxo(m_txDesc.m_amount, currentHeight);
            LOG_INFO() << m_txDesc.m_txId << " Invitation accepted";
            updateTxDescription(TxDescription::InProgress);
        }
        Scalar peerSignature;
        if (!getParameter(TxParams::PeerSignature, peerSignature))
        {
            sendConfirmInvitation();
            return;
        }

        bool isRegistered = false;
        if (!getParameter(TxParams::TransactionRegistered, isRegistered))
        {
            if (!registerTxInternal(peerSignature))
            {
                onFailed(true);
                return;
            }
            updateTxDescription(TxDescription::InProgress);
            sendNewTransaction();
            return;
        }

        if (!isRegistered)
        {
            onFailed(true);
            return;
        }
        
        bool isConfirmed = false;
        if (!getParameter(TxParams::TransactionConfirmed, isConfirmed))
        {
            m_gateway.send_tx_registered(m_txDesc);
            confirmOutputs();
            return;
        }

        completeTx();
    }

    void ReceiveTransaction::confirmInvitation()
    {
        updateTxDescription(TxDescription::Pending);
        bool sender = m_txDesc.m_sender;
        LOG_INFO() << m_txDesc.m_txId << (sender ? " Sending " : " Receiving ") << PrintableAmount(m_txDesc.m_amount) << " (fee: " << PrintableAmount(m_txDesc.m_fee) << ")";
        Height currentHeight = m_keychain->getCurrentHeight();

        createKernel(m_txDesc.m_fee, m_txDesc.m_minHeight);

        if (sender)
        {
            if (!prepareSenderUtxos(currentHeight))
            {
                onFailed(true);
                return;
            }
        }
        else
        {
            createOutputUtxo(m_txDesc.m_amount, currentHeight);
        }

        LOG_INFO() << m_txDesc.m_txId << " Invitation accepted";
        updateTxDescription(TxDescription::InProgress);
        sendConfirmInvitation();
    }

    void ReceiveTransaction::sendConfirmInvitation() const
    {
        ConfirmInvitation confirmMsg;
        confirmMsg.m_txId = m_txDesc.m_txId;
        confirmMsg.m_publicPeerExcess = getPublicExcess();
        NoLeak<Scalar> t;
        createSignature2(confirmMsg.m_peerSignature, confirmMsg.m_publicPeerNonce, t.V);

        m_gateway.send_tx_confirmation(m_txDesc, move(confirmMsg));
    }

    void ReceiveTransaction::registerTx()
    {
        Scalar peerSignature;
        if (!getParameter(TxParams::PeerSignature, peerSignature))
        {
            return;
        }

        if (!registerTxInternal(peerSignature))
        {
            onFailed(true);
            return;
        }
        updateTxDescription(TxDescription::InProgress);
        sendNewTransaction();
    }

    bool BaseTransaction::registerTxInternal(const ECC::Scalar& peerSignature)
    {
        if (!isValidSignature(peerSignature))
            return false;

        // Calculate final signature
        Scalar::Native senderSignature;
        senderSignature = peerSignature;
        Scalar::Native receiverSignature = createSignature();
        Scalar::Native finialSignature = senderSignature + receiverSignature;
        return constructTxInternal(finialSignature);
    }

    bool BaseTransaction::constructTxInternal(const Scalar::Native& signature)
    {
        // Create transaction kernel and transaction
        m_kernel->m_Signature.m_k = signature;
        m_transaction = make_shared<Transaction>();
        m_transaction->m_vKernelsOutput.push_back(move(m_kernel));
        m_transaction->m_Offset = m_offset;
        getParameter(TxParams::PeerInputs, m_transaction->m_vInputs);
        getParameter(TxParams::PeerOutputs, m_transaction->m_vOutputs);

        {
            auto inputs = getTxInputs(m_txDesc.m_txId);
            move(inputs.begin(), inputs.end(), back_inserter(m_transaction->m_vInputs));

            auto outputs = getTxOutputs(m_txDesc.m_txId);
            move(outputs.begin(), outputs.end(), back_inserter(m_transaction->m_vOutputs));
        }

        m_transaction->Sort();

        // Verify final transaction
        TxBase::Context ctx;
        return m_transaction->IsValid(ctx);
    }

    void BaseTransaction::sendNewTransaction() const
    {
        m_gateway.register_tx(m_txDesc, m_transaction);
    }

    //void Negotiator::FSMDefinition::rollbackTx(const events::TxFailed& event)
    //{
    //    updateTxDescription(TxDescription::Failed);
    //    rollbackTx();
    //    if (event.m_notify)
    //    {
    //        m_parent.m_gateway.send_tx_failed(m_parent.m_txDesc);
    //    }
    //}

    //void Negotiator::FSMDefinition::cancelTx(const events::TxCanceled&)
    //{
    //    if (m_parent.m_txDesc.m_status == TxDescription::Pending)
    //    {
    //        m_parent.m_keychain->deleteTx(m_parent.m_txDesc.m_txId);
    //    }
    //    else
    //    {
    //        updateTxDescription(TxDescription::Cancelled);
    //        rollbackTx();
    //        m_parent.m_gateway.send_tx_failed(m_parent.m_txDesc);
    //    }
    //}

    void BaseTransaction::rollbackTx()
    {
        LOG_INFO() << m_txDesc.m_txId << " Transaction failed. Rollback...";
        m_keychain->rollbackTx(m_txDesc.m_txId);
    }

    void BaseTransaction::confirmOutputs()
    {
        LOG_INFO() << m_txDesc.m_txId << " Transaction registered";
        updateTxDescription(TxDescription::Registered);

        auto coins = m_keychain->getCoinsCreatedByTx(m_txDesc.m_txId);

        for (auto& coin : coins)
        {
            coin.m_status = Coin::Unconfirmed;
        }
        m_keychain->update(coins);

        m_gateway.confirm_outputs(m_txDesc);
    }

    void BaseTransaction::completeTx()
    {
        LOG_INFO() << m_txDesc.m_txId << " Transaction completed";
        updateTxDescription(TxDescription::Completed);
    }

    void BaseTransaction::updateTxDescription(TxDescription::Status s)
    {
        m_txDesc.m_status = s;
        m_txDesc.m_modifyTime = getTimestamp();
    }

    bool BaseTransaction::prepareSenderUtxos(const Height& currentHeight)
    {
        Amount amountWithFee = m_txDesc.m_amount + m_txDesc.m_fee;
        auto coins = m_keychain->selectCoins(amountWithFee);
        if (coins.empty())
        {
            LOG_ERROR() << "You only have " << PrintableAmount(getAvailable(m_keychain));
            return false;
        }
        for (auto& coin : coins)
        {
            Scalar::Native blindingFactor = m_keychain->calcKey(coin);
            m_blindingExcess += blindingFactor;
            coin.m_spentTxId = m_txDesc.m_txId;
        }
        m_keychain->update(coins);
        // calculate change amount and create corresponding output if needed
        Amount change = 0;
        for (const auto &coin : coins)
        {
            change += coin.m_amount;
        }
        change -= amountWithFee;
        if (change > 0)
        {
            createOutputUtxo(change, currentHeight);
            m_txDesc.m_change = change;
        }
        return true;
    }

    void BaseTransaction::createKernel(Amount fee, Height minHeight)
    {
        m_kernel = make_unique<TxKernel>();
        m_kernel->m_Fee = fee;
        m_kernel->m_Height.m_Min = minHeight;
        m_kernel->m_Height.m_Max = MaxHeight;
        m_kernel->m_Excess = Zero;
    }

    void BaseTransaction::createOutputUtxo(Amount amount, Height height)
    {
        Coin newUtxo{ amount, Coin::Draft, height };
        newUtxo.m_createTxId = m_txDesc.m_txId;
        m_keychain->store(newUtxo);

        Scalar::Native blindingFactor = m_keychain->calcKey(newUtxo);
        auto[privateExcess, offset] = splitKey(blindingFactor, newUtxo.m_id);

        blindingFactor = -privateExcess;
        m_blindingExcess += blindingFactor;
        m_offset += offset;
    }

    bool BaseTransaction::ProcessInvitation(Invite& inviteMsg)
    {
        if (!m_publicPeerExcess.Import(inviteMsg.m_publicPeerExcess) ||
            !m_publicPeerNonce.Import(inviteMsg.m_publicPeerNonce))
            return false;

        m_offset = inviteMsg.m_offset;
        m_transaction = std::make_shared<Transaction>();
        m_transaction->m_Offset = Zero;
        m_transaction->m_vInputs = move(inviteMsg.m_inputs);
        m_transaction->m_vOutputs = move(inviteMsg.m_outputs);

        return true;
    }

    Scalar BaseTransaction::createSignature()
    {
        Point publicNonce;
        Scalar partialSignature;
        createSignature2(partialSignature, publicNonce, m_kernel->m_Signature.m_e);
        return partialSignature;
    }

    void BaseTransaction::get_NonceInternal(ECC::Signature::MultiSig& out) const
    {
        Point pt = m_kernel->m_Excess;
        m_kernel->m_Excess = Zero;

        Hash::Value hv;
        m_kernel->get_Hash(hv);

        m_kernel->m_Excess = pt;

        out.GenerateNonce(hv, m_blindingExcess);
    }

    void BaseTransaction::onFailed(bool notify)
    {
        updateTxDescription(TxDescription::Failed);
        rollbackTx();
        if (notify)
        {
            m_gateway.send_tx_failed(m_txDesc);
        }
        m_gateway.on_tx_completed(m_txDesc);
    }

    void BaseTransaction::createSignature2(Scalar& signature, Point& publicNonce, Scalar& challenge) const
    {
        Signature::MultiSig msig;
        get_NonceInternal(msig);

        Point::Native pt = Context::get().G * msig.m_Nonce;
        publicNonce = pt;
        msig.m_NoncePub = m_publicPeerNonce + pt;

        pt = Context::get().G * m_blindingExcess;
        pt += m_publicPeerExcess;
        m_kernel->m_Excess = pt;
        Hash::Value message;
        m_kernel->get_Hash(message);

        Scalar::Native partialSignature;
        Signature sig;
        sig.CoSign(partialSignature, message, m_blindingExcess, msig);
        challenge = sig.m_e;
        signature = partialSignature;
    }

    Point BaseTransaction::getPublicExcess() const
    {
        return Point(Context::get().G * m_blindingExcess);
    }

    Point BaseTransaction::getPublicNonce() const
    {
        Signature::MultiSig msig;
        get_NonceInternal(msig);

        return Point(Context::get().G * msig.m_Nonce);
    }

    bool BaseTransaction::isValidSignature(const Scalar& peerSignature) const
    {
        return isValidSignature(peerSignature, m_publicPeerNonce, m_publicPeerExcess);
    }

    bool BaseTransaction::isValidSignature(const Scalar& peerSignature, const Point& publicPeerNonce, const Point& publicPeerExcess) const
    {
        //assert(m_kernel);
        if (!m_kernel)
            return false;

        Signature::MultiSig msig;
        get_NonceInternal(msig);

        Point::Native publicNonce = Context::get().G * msig.m_Nonce;

        Point::Native pkPeer, xcPeer;
        if (!pkPeer.Import(publicPeerNonce) ||
            !xcPeer.Import(publicPeerExcess))
            return false;

        msig.m_NoncePub = publicNonce + pkPeer;

        Point::Native pt = Context::get().G * m_blindingExcess;
        pt += xcPeer;
        m_kernel->m_Excess = pt;

        Hash::Value message;
        m_kernel->get_Hash(message);

        // temp signature to calc challenge
        Scalar::Native mySig;
        Signature peerSig;
        peerSig.CoSign(mySig, message, m_blindingExcess, msig);
        peerSig.m_k = peerSignature;
        return peerSig.IsValidPartial(pkPeer, xcPeer);
    }

    vector<Input::Ptr> BaseTransaction::getTxInputs(const TxID& txID) const
    {
        vector<Input::Ptr> inputs;
        m_keychain->visit([this, &txID, &inputs](const Coin& c)->bool
        {
            if (c.m_spentTxId == txID && c.m_status == Coin::Locked)
            {
                Input::Ptr input = make_unique<Input>();

                Scalar::Native blindingFactor = m_keychain->calcKey(c);
                input->m_Commitment = Commitment(blindingFactor, c.m_amount);

                inputs.push_back(move(input));
            }
            return true;
        });
        return inputs;
    }

    vector<Output::Ptr> BaseTransaction::getTxOutputs(const TxID& txID) const
    {
        vector<Output::Ptr> outputs;
        m_keychain->visit([this, &txID, &outputs](const Coin& c)->bool
        {
            if (c.m_createTxId == txID && c.m_status == Coin::Draft)
            {
                Output::Ptr output = make_unique<Output>();
                output->m_Coinbase = false;

                Scalar::Native blindingFactor = m_keychain->calcKey(c);
                output->Create(blindingFactor, c.m_amount);

                outputs.push_back(move(output));
            }
            return true;
        });
        return outputs;
    }
}}
