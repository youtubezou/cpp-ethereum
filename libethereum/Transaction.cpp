/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Transaction.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include <libdevcore/vector_ref.h>
#include <libdevcore/Log.h>
#include <libdevcore/CommonIO.h>
#include <libdevcrypto/Common.h>
#include <libethcore/Exceptions.h>
#include <libevm/VMFace.h>
#include "Interface.h"
#include "Transaction.h"
using namespace std;
using namespace dev;
using namespace dev::eth;

#define ETH_ADDRESS_DEBUG 0

std::ostream& dev::eth::operator<<(std::ostream& _out, ExecutionResult const& _er)
{
	_out << "{" << _er.gasUsed << ", " << _er.newAddress << ", " << toHex(_er.output) << "}";
	return _out;
}

TransactionException dev::eth::toTransactionException(Exception const& _e)
{
	// Basic Transaction exceptions
	if (!!dynamic_cast<BadRLP const*>(&_e))
		return TransactionException::BadRLP;
	if (!!dynamic_cast<OutOfGasIntrinsic const*>(&_e))
		return TransactionException::OutOfGasIntrinsic;
	if (!!dynamic_cast<InvalidSignature const*>(&_e))
		return TransactionException::InvalidSignature;
	// Executive exceptions
	if (!!dynamic_cast<OutOfGasBase const*>(&_e))
		return TransactionException::OutOfGasBase;
	if (!!dynamic_cast<InvalidNonce const*>(&_e))
		return TransactionException::InvalidNonce;
	if (!!dynamic_cast<NotEnoughCash const*>(&_e))
		return TransactionException::NotEnoughCash;
	if (!!dynamic_cast<BlockGasLimitReached const*>(&_e))
		return TransactionException::BlockGasLimitReached;
	// VM execution exceptions
	if (!!dynamic_cast<BadInstruction const*>(&_e))
		return TransactionException::BadInstruction;
	if (!!dynamic_cast<BadJumpDestination const*>(&_e))
		return TransactionException::BadJumpDestination;
	if (!!dynamic_cast<OutOfGas const*>(&_e))
		return TransactionException::OutOfGas;
	if (!!dynamic_cast<OutOfStack const*>(&_e))
		return TransactionException::OutOfStack;
	if (!!dynamic_cast<StackUnderflow const*>(&_e))
		return TransactionException::StackUnderflow;
	return TransactionException::Unknown;
}

std::ostream& dev::eth::operator<<(std::ostream& _out, TransactionException const& _er)
{
	switch (_er)
	{
		case TransactionException::None: _out << "None"; break;
		case TransactionException::BadRLP: _out << "BadRLP"; break;
		case TransactionException::OutOfGasIntrinsic: _out << "OutOfGasIntrinsic"; break;
		case TransactionException::InvalidSignature: _out << "InvalidSignature"; break;
		case TransactionException::InvalidNonce: _out << "InvalidNonce"; break;
		case TransactionException::NotEnoughCash: _out << "NotEnoughCash"; break;
		case TransactionException::OutOfGasBase: _out << "OutOfGasBase"; break;
		case TransactionException::BlockGasLimitReached: _out << "BlockGasLimitReached"; break;
		case TransactionException::BadInstruction: _out << "BadInstruction"; break;
		case TransactionException::BadJumpDestination: _out << "BadJumpDestination"; break;
		case TransactionException::OutOfGas: _out << "OutOfGas"; break;
		case TransactionException::OutOfStack: _out << "OutOfStack"; break;
		case TransactionException::StackUnderflow: _out << "StackUnderflow"; break;
		default: _out << "Unknown"; break;
	}
	return _out;
}

Transaction::Transaction(bytesConstRef _rlpData, CheckTransaction _checkSig)
{
	int field = 0;
	RLP rlp(_rlpData);
	try
	{
		if (!rlp.isList())
			BOOST_THROW_EXCEPTION(BadRLP() << errinfo_comment("transaction RLP must be a list"));

		m_nonce = rlp[field = 0].toInt<u256>();
		m_gasPrice = rlp[field = 1].toInt<u256>();
		m_gas = rlp[field = 2].toInt<u256>();
		m_type = rlp[field = 3].isEmpty() ? ContractCreation : MessageCall;
		m_receiveAddress = rlp[field = 3].isEmpty() ? Address() : rlp[field = 3].toHash<Address>(RLP::VeryStrict);
		m_value = rlp[field = 4].toInt<u256>();

		if (!rlp[field = 5].isData())
			BOOST_THROW_EXCEPTION(BadRLP() << errinfo_comment("transaction data RLP must be an array"));

		m_data = rlp[field = 5].toBytes();
		byte v = rlp[field = 6].toInt<byte>() - 27;
		h256 r = rlp[field = 7].toInt<u256>();
		h256 s = rlp[field = 8].toInt<u256>();

		if (rlp.itemCount() > 9)
			BOOST_THROW_EXCEPTION(BadRLP() << errinfo_comment("to many fields in the transaction RLP"));

		m_vrs = SignatureStruct{ r, s, v };
		if (_checkSig >= CheckTransaction::Cheap && !m_vrs.isValid())
			BOOST_THROW_EXCEPTION(InvalidSignature());
		if (_checkSig == CheckTransaction::Everything)
			m_sender = sender();
	}
	catch (Exception& _e)
	{
		_e << errinfo_name("invalid transaction format") << BadFieldError(field, toHex(rlp[field].data().toBytes()));
		throw;
	}
	if (_checkSig >= CheckTransaction::Cheap && !checkPayment())
		BOOST_THROW_EXCEPTION(OutOfGasIntrinsic() << RequirementError(gasRequired(), (bigint)gas()));
}

Address const& Transaction::safeSender() const noexcept
{
	try
	{
		return sender();
	}
	catch (...)
	{
		cwarn << "safeSender() did throw an exception: " <<  boost::current_exception_diagnostic_information();
		return ZeroAddress;
	}
}

Address const& Transaction::sender() const
{
	if (!m_sender)
	{
		auto p = recover(m_vrs, sha3(WithoutSignature));
		if (!p)
			BOOST_THROW_EXCEPTION(InvalidSignature());
		m_sender = right160(dev::sha3(bytesConstRef(p.data(), sizeof(p))));
	}
	return m_sender;
}

bigint Transaction::gasRequired() const
{
	if (!m_gasRequired)
		m_gasRequired = Transaction::gasRequired(m_data);
	return m_gasRequired;
}

void Transaction::sign(Secret _priv)
{
	auto sig = dev::sign(_priv, sha3(WithoutSignature));
	SignatureStruct sigStruct = *(SignatureStruct const*)&sig;
	if (sigStruct.isValid())
		m_vrs = sigStruct;
}

void Transaction::streamRLP(RLPStream& _s, IncludeSignature _sig) const
{
	if (m_type == NullTransaction)
		return;
	_s.appendList((_sig ? 3 : 0) + 6);
	_s << m_nonce << m_gasPrice << m_gas;
	if (m_type == MessageCall)
		_s << m_receiveAddress;
	else
		_s << "";
	_s << m_value << m_data;
	if (_sig)
		_s << (m_vrs.v + 27) << (u256)m_vrs.r << (u256)m_vrs.s;
}
