//
// Created by mctrivia on 17/06/23.
//

#include <chrono>
#include <cmath>
#include <iostream>
#include "DigiByteTransaction.h"
#include "BitIO.h"
#include "DigiAsset.h"
#include "IPFS.h"
#include "DigiByteDomain.h"

using namespace std;



DigiByteTransaction::DigiByteTransaction() {
    _time = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
}


/**
 * Creates an object that can hold a transactions data including any DigiAssets
 * height is optional but if known will speed things up if provided
 */
DigiByteTransaction::DigiByteTransaction(const string& txid, DigiByteCore& core, unsigned int height) {
    getrawtransaction_t txData = core.getRawTransaction(txid);
    if (height == 0) height = core.getBlock(txData.blockhash).height;

    Database* db = Database::GetInstance();
    _height = height;
    _blockHash = txData.blockhash;
    _time = txData.time;
    _txid = txData.txid;

    //copy input data
    _assetFound = false;
    for (const vin_t& vin: txData.vin) {
        //if a coinbase transaction don't look up the input
        if (vin.txid.empty()) {
            if (txData.vin.size() != 1) { //todo think only 1 input allowed but check
                cout << "*";
            }
            break;
        }

        //find any assets on input utxos
        AssetUTXO input = db->getAssetUTXO(vin.txid, vin.n);
        if (!input.assets.empty()) _assetFound = true;
        _inputs.push_back(input);
    }

    //copy output data
    for (const vout_t& vout: txData.vout) {
        _outputs.emplace_back(AssetUTXO{
                .txid=txData.txid,
                .vout=static_cast<uint16_t>(vout.n),
                .address=(vout.scriptPubKey.addresses.empty()) ? "" : vout.scriptPubKey.addresses[0],
                .digibyte=vout.valueS
        });
    }

    //Check different tx types
    if (processKYC(txData)) return;
    if (processExchangeRate(txData)) return;
    processAssetTX(txData);
}

bool DigiByteTransaction::processExchangeRate(const getrawtransaction_t& txData) {
    //exchange rate transactions always have 1 input and 2 outputs
    if (txData.vin.size() != 1) return false;
    if (txData.vout.size() != 2) return false;

    //check first output is an op_return
    if (txData.vout[0].scriptPubKey.type != "nulldata") return false;
    BitIO dataStream = BitIO::makeHexString(txData.vout[0].scriptPubKey.hex);
    if (!dataStream.checkIsBitcoinOpReturn()) return false;   //not an OP_RETURN
    if (dataStream.getBitcoinDataHeader() != BITIO_BITCOIN_TYPE_DATA) return false; //not data
    dataStream = dataStream.copyBitcoinData();    //strip the header out

    //check data is a multiple of 8 bytes
    if (dataStream.getLength() % 64 != 0) return false;

    //check only 1 address in output 2, and it is an exchange address
    if (txData.vout[1].scriptPubKey.addresses.size() != 1) return false;
    Database* db = Database::GetInstance();
    string address = txData.vout[1].scriptPubKey.addresses[0];
    if (!db->isWatchAddress(address)) return false;

    //check input utxo is from same address as output 2
    if (address != db->getSendingAddress(txData.vin[0].txid, txData.vin[0].n)) return false;

    //if we get to this line the transaction is an exchange rate transaction

    //record exchange rates
    while (dataStream.getPosition() != dataStream.getLength()) {
        _exchangeRate.emplace_back(dataStream.getDouble());
    }
    _txType = EXCHANGE_PUBLISH;

    return true;
}

bool DigiByteTransaction::processKYC(const getrawtransaction_t& txData) {
    unsigned int txType = _kycData.processTX(txData, _height, [this](string txid, unsigned int vout) -> string {
        Database* db = Database::GetInstance();
        return db->getSendingAddress(txid, vout);
    });
    if (txType == KYC::NA) return false;
    if (txType == KYC::VERIFY) {
        _txType = KYC_ISSUANCE;
    } else {    //KYC::REVOKE
        _txType = KYC_REVOKE;
    }
    return true;
}

void DigiByteTransaction::processAssetTX(const getrawtransaction_t& txData) {


    //find the encoded data
    int iO = -1;
    for (const vout_t& output: txData.vout) {
        if (output.scriptPubKey.type != "nulldata") continue;
        iO = output.n;
    }
    if (iO == -1) {
        return;
    }

    //check encoded data on output 1 has correct header
    BitIO dataStream = BitIO::makeHexString(txData.vout[iO].scriptPubKey.hex);
    if (dataStream.getLength() < DIGIASSET_MIN_POSSIBLE_LENGTH) {
        return;   //fc8ac69d67c298152a8b93b1b7a054e28427f02e69025249e09be123de2986f3 has an OP_RETURN with no extra data after.  This prevents error
    }
    if (!dataStream.checkIsBitcoinOpReturn()) {
        return;   //not an OP_RETURN
    }
    if (dataStream.getBitcoinDataHeader() != BITIO_BITCOIN_TYPE_DATA) {
        return; //not data
    }
    dataStream = dataStream.copyBitcoinData();    //strip the header out

    if (dataStream.getBits(16) != 0x4441) {
        return; //not asset tx
    }

    //get version number
    _assetTransactionVersion = dataStream.getBits(8);

    //get opcode
    const unsigned char opcode = dataStream.getBits(8);
    if (opcode == 0) {
        return;//invalid op code
    }
    if (opcode < 16) {

        //try to build the DigiAsset Object that is encoded in chain
        try {

            //check if any assets where burnt since assets are not transferred during an issuance
            if (_assetFound) _unintentionalBurn = true;

            //create the asset
            _newAsset = DigiAsset{txData, _height, _assetTransactionVersion, opcode, dataStream};
            processAssetTransfer(dataStream, vector<AssetUTXO>{{
                                                                       .digibyte =  0,
                                                                       .assets =  vector<DigiAsset>{_newAsset}
                                                               }}, DIGIASSET_ISSUANCE);
            _txType = DIGIASSET_ISSUANCE;
        } catch (const DigiAsset::exception& e) {

        }
        return;


    } else if (opcode < 48) {

        //check if valid transfer or burn
        bool burn = (opcode >= 0x20);
        if ((opcode % 16) != 5) {
            return;    //invalid transfer opcode
        }

        if (!_assetFound) {
            return;
        }

        //do transfer
        try {
            processAssetTransfer(dataStream, _inputs, burn ? DIGIASSET_BURN : DIGIASSET_TRANSFER);
        } catch (const DigiAsset::exceptionRuleFailed& e) {
            //clear asset outputs
            _unintentionalBurn = true;
            for (AssetUTXO& output: _outputs) {
                output.assets.clear();
            }
            return;
        } catch (const DigiAsset::exception& e) {
            return;
        } catch (const out_of_range& e) {
            return;
        }
        _txType = burn ? DIGIASSET_BURN : DIGIASSET_TRANSFER;
    }
}

void DigiByteTransaction::processAssetTransfer(BitIO& dataStream, const vector<AssetUTXO>& inputAssets, uint8_t type) {
    bool allowSkip = true;
    size_t index = 0;

    //get list of input assets
    vector<vector<DigiAsset>> inputs;
    for (const AssetUTXO& vin: inputAssets) {
        if (vin.assets.empty()) continue;
        inputs.emplace_back(vin.assets); //copy the assets
    }

    //read transfer instructions
    size_t footerBitCount = (type == DIGIASSET_ISSUANCE) ? 8 : 0;
    while (dataStream.getNumberOfBitLeft() > footerBitCount) {
        //read the instruction
        bool skip = dataStream.getBits(1);
        bool range = dataStream.getBits(1);
        bool percent = dataStream.getBits(1);
        uint16_t output = range ? dataStream.getBits(13) : dataStream.getBits(5);
        uint64_t amount = percent ? inputs[index][0].getCount() * percent / 100 : dataStream.getFixedPrecision();
        uint64_t totalAmount = range ? (output + 1) * amount : amount;

        //there was an error in legacy code that a 0 amount causes the input to get wasted and go to change
        if ((_assetTransactionVersion < 3) && (type != DIGIASSET_ISSUANCE) && (_inputs[0].assets.empty())) {
            break;
        }

        //remove assets from input
        try {

            //check that input has assets
            if ((index >= inputs.size()) || (inputs[index].empty())) {
                throw DigiAsset::exceptionInvalidTransfer();
            } //Request from input with no assets

            //get
            uint64_t leftToRemoveFromInputs = totalAmount;
            DigiAsset removedAsset = inputs[index][0];
            while (leftToRemoveFromInputs > 0) {
                //check that input has assets
                if ((index >= inputs.size()) || (inputs[index].empty())) {
                    throw DigiAsset::exceptionInvalidTransfer();
                } //Request from input with no assets

                //get number available
                uint64_t currentAmount = inputs[index][0].getCount();

                //check asset id matched
                if (inputs[index][0] != removedAsset) {
                    throw DigiAsset::exceptionInvalidTransfer();
                } //Different asset then expected;

                //see if we used them all up
                allowSkip = true;   //make sure true for all instructions except where ends on exactly 0 leftToRemoveFromInputs
                if ((currentAmount < leftToRemoveFromInputs) && (inputs[index][0].isHybrid())) {
                    throw DigiAsset::exceptionInvalidTransfer();
                } //"Hybrid assets can't rap over inputs;
                if (currentAmount <= leftToRemoveFromInputs) {
                    //used all assets in the input up
                    leftToRemoveFromInputs -= currentAmount;
                    inputs[index].erase(inputs[index].begin());
                    if (inputs[index].empty()) {
                        //used all inputs up so move to next
                        index++;
                        allowSkip = false;//exactly 0 leftToRemoveFromInputs so disable skip
                    }
                } else {
                    //there are assets leftToRemoveFromInputs in the input
                    inputs[index][0].removeCount(leftToRemoveFromInputs);
                    leftToRemoveFromInputs = 0;
                }
            }

            //apply removed assets to outputs
            if (totalAmount > 0) {
                if ((type == DIGIASSET_BURN) && (!range) && (output == 31)) {
                    //burn asset so do nothing
                } else {
                    size_t startI = range ? 0 : output;
                    removedAsset.setCount(amount);
                    if (output >= _outputs.size()) {
                        throw DigiAsset::exceptionInvalidTransfer();
                    } //Tried to send to an output that doesn't exist
                    for (size_t vout = startI; vout <= output; vout++) {
                        addAssetToOutput(vout, removedAsset);
                    }
                }
            }

            //skip remainder of vin inputs if called for
            if (skip) {
                if (allowSkip) index++;     //ignore skip if last instruction emptied the input
                allowSkip = true;
            }
        } catch (const exception& e) {
            //remove any assets that where already applied
            for (AssetUTXO& vout: _outputs) {
                vout.assets.clear();
            }

            //reset inputs
            inputs.clear();
            for (const AssetUTXO& vin: inputAssets) {
                if (vin.assets.empty()) continue;
                inputs.emplace_back(vin.assets); //copy the assets
            }

            break;  //breaks the while (dataStream.getNumberOfBitLeft() > footerBitCount) loop
        }

    }

    //see if any change
    size_t lastOutput = _outputs.size() - 1;
    for (const vector<DigiAsset>& input: inputs) {
        for (const DigiAsset& asset: input) {
            //check there is something there(sometimes count 0)
            if (asset.getCount() == 0) {
                continue;
            }

            //something left over so see if already in list
            bool needAdding = true;
            if (asset.isAggregable()) {   //only search on aggregable
                for (DigiAsset& assetTest: _outputs[lastOutput].assets) {
                    if (assetTest.getAssetIndex() == asset.getAssetIndex()) {
                        //already there so add the amount
                        assetTest.addCount(asset.getCount());
                        needAdding = false;
                        break;
                    }
                }
            }

            //not found so add
            if (needAdding) addAssetToOutput(lastOutput, asset);
        }
    }

    //check rules where followed if there were any
    if (type != DIGIASSET_ISSUANCE) {
        checkRulesPass();
    }

    //make sure no outputs with assets are op_return data
    for (AssetUTXO& output: _outputs) {
        if ((output.address == "") && (!output.assets.empty())) {
            output.assets.clear();
            _unintentionalBurn = true;
        }
    }
}

void DigiByteTransaction::checkRulesPass() const {
    for (const AssetUTXO& utxo: _inputs) {
        for (const DigiAsset& asset: utxo.assets) asset.checkRulesPass(_inputs, _outputs, _height, _time);
    }
}

AssetUTXO DigiByteTransaction::getInput(size_t n) const {
    return _inputs[n];
}

AssetUTXO DigiByteTransaction::getOutput(size_t n) const {
    return _outputs[n];
}


bool DigiByteTransaction::isStandardTransaction() const {
    return (_txType == STANDARD);
}

bool DigiByteTransaction::isNonAssetTransaction() const {
    if (isUnintentionalBurn()) return false;
    return ((_txType < DIGIASSET_ISSUANCE) || (_txType > DIGIASSET_BURN));
}

bool DigiByteTransaction::isIssuance() const {
    return (_txType == DIGIASSET_ISSUANCE);
}

bool DigiByteTransaction::isTransfer(bool includeIntentionalBurn) const {
    if (_txType == DIGIASSET_TRANSFER) return true;
    if (includeIntentionalBurn && (_txType == DIGIASSET_BURN)) return true;
    return false;
}

bool DigiByteTransaction::isBurn(bool includeUnintentionalBurn) const {
    if (_txType == DIGIASSET_BURN) return true;
    if (!includeUnintentionalBurn) return false;
    return isUnintentionalBurn();
}

bool DigiByteTransaction::isUnintentionalBurn() const {
    if (_unintentionalBurn) return true;
    if (_txType != STANDARD) return false;
    return _assetFound;
}

bool DigiByteTransaction::isKYCTransaction() const {
    return ((_txType == KYC_REVOKE) || (_txType == KYC_ISSUANCE));
}

bool DigiByteTransaction::isKYCRevoke() const {
    return (_txType == KYC_REVOKE);
}

bool DigiByteTransaction::isKYCIssuance() const {
    return (_txType == KYC_ISSUANCE);
}

KYC DigiByteTransaction::getKYC() const {
    return _kycData;
}

bool DigiByteTransaction::isExchangeTransaction() const {
    return (_txType == EXCHANGE_PUBLISH);
}

size_t DigiByteTransaction::getExchangeRateCount() const {
    return _exchangeRate.size();
}

double DigiByteTransaction::getExchangeRate(uint8_t i) const {
    if (i >= _exchangeRate.size()) throw out_of_range("Non existent exchange rate");
    return _exchangeRate[i];
}

ExchangeRate DigiByteTransaction::getExchangeRateName(uint8_t i) const {
    if (i >= _exchangeRate.size()) throw out_of_range("Non existent exchange rate");
    string name;
    for (size_t offset = 0; offset < DigiAsset::standardExchangeRatesCount; offset += 10) {
        if (_outputs[1].address == DigiAsset::standardExchangeRates[offset].address) {
            name = DigiAsset::standardExchangeRates[offset + i].name;
        }
    }
    return {
            .address= _outputs[1].address,
            .index=i,
            .name=name
    };
}


void DigiByteTransaction::addAssetToOutput(size_t output, const DigiAsset& asset) {
    //see if asset already in output
    if (asset.isAggregable() && (!_outputs[output].assets.empty())) {
        for (DigiAsset& existingOutput: _outputs[output].assets) {
            if (existingOutput.getAssetIndex() == asset.getAssetIndex()) {
                existingOutput.addCount(asset.getCount());
                return;
            }
        }
    }

    //add asset to end of assets on the output
    _outputs[output].assets.emplace_back(asset);
}

void DigiByteTransaction::addToDatabase(const std::string& optionalMetaCallbackSymbol) {
    Database* db = Database::GetInstance();

    //set to process all changes at once
    db->startTransaction();

    //add special tx types
    switch (_txType) {
        case KYC_ISSUANCE:
            db->addKYC(_kycData.getAddress(), _kycData.getCountry(), _kycData.getName(), _kycData.getHash(), _height);

            break;
        case KYC_REVOKE:
            db->revokeKYC(_kycData.getAddress(), _height);
            break;
        case EXCHANGE_PUBLISH:
            for (size_t index = 0; index < _exchangeRate.size(); index++) {
                if (isnan(_exchangeRate[index])) continue;
                db->addExchangeRate(_outputs[1].address, index, _height, _exchangeRate[index]);
            }
            break;
        case DIGIASSET_ISSUANCE:
            bool indexAlreadySet = (_newAsset.getAssetIndex() != 0);
            uint64_t assetIndex = db->addAsset(_newAsset);                              //add asset to the database
            IPFS* ipfs = IPFS::GetInstance();
            ipfs->pin(
                    _newAsset.getCID());    //add the cid to the lookup list to make sure we have a local copy
            if (!optionalMetaCallbackSymbol.empty()) {
                ipfs->callOnDownload(_newAsset.getCID(), "", _txid, optionalMetaCallbackSymbol);
            }
            DigiByteDomain::processAssetIssuance(
                    _newAsset);       //allow digibyte domain changes to be handled
            if (indexAlreadySet) break;

            //set that assetIndex on the outputs
            for (AssetUTXO& vout: _outputs) {
                if (vout.assets.empty()) continue;
                for (DigiAsset& asset: vout.assets) {
                    asset.setAssetIndex(assetIndex);
                }
            }
            break;
    }

    //mark spent old UTXOs
    for (const AssetUTXO& vin: _inputs) {
        if (vin.txid == "") continue;  //coinbase
        db->spendUTXO(vin.txid, vin.vout, _height);
    }

    //add utxos
    for (const AssetUTXO& vout: _outputs) {
        db->createUTXO(vout, _height);
    }

    //handle votes
    for (const AssetUTXO& vout: _outputs) {
        if (vout.assets.empty()) continue;
        for (const DigiAsset& asset: vout.assets) {
            if (!asset.getRules().getIfValidVoteAddress(vout.address)) continue;
            db->addVote(vout.address, asset.getAssetIndex(), asset.getCount(), _height);
        }
    }

    //finalise changes
    db->endTransaction();
}

Value DigiByteTransaction::toJSON(const Value& original) const {
    Json::Value result = original;
    bool addingToOriginal = !original.empty();

    // Get basic data
    result["txid"] = _txid;
    result["blockhash"] = _blockHash;
    result["height"] = _height;
    result["time"] = _time;

    // Add vin data
    Json::Value inputsArray(Json::arrayValue);
    for (size_t i = 0; i < _inputs.size(); i++) {
        const AssetUTXO& input = _inputs[i];
        Json::Value inputObject = addingToOriginal ? result["vin"][static_cast<Json::ArrayIndex>(i)]
                                                   : Json::objectValue;   //load original or blank
        inputObject["txid"] = input.txid;
        inputObject["vout"] = input.vout;
        inputObject["address"] = input.address;
        inputObject["valueS"] = input.digibyte;

        Json::Value assetArray(Json::arrayValue);
        for (const DigiAsset& asset: input.assets) {
            assetArray.append(asset.toJSON(true));
        }
        inputObject["assets"] = assetArray;
        inputsArray.append(inputObject);
    }
    result["vin"] = inputsArray;

    // Add vout data
    Json::Value outputArray(Json::arrayValue);
    for (size_t i = 0; i < _outputs.size(); i++) {
        const AssetUTXO& output = _outputs[i];
        Json::Value outputObject = addingToOriginal ? result["vout"][static_cast<Json::ArrayIndex>(i)]
                                                    : Json::objectValue;   //load original or blank
        outputObject["n"] = output.vout;
        outputObject["address"] = output.address;
        outputObject["valueS"] = output.digibyte;

        Json::Value assetArray(Json::arrayValue);
        for (const DigiAsset& asset: output.assets) {
            assetArray.append(asset.toJSON(true));
        }
        outputObject["assets"] = assetArray;
        outputArray.append(outputObject);
    }
    result["vout"] = outputArray;

    // Add issuance
    if (isIssuance()) {
        DigiAsset issuedAsset = getIssuedAsset();
        result["issued"] = issuedAsset.toJSON();
    }

    // Add exchange data
    if (isExchangeTransaction()) {
        size_t count = getExchangeRateCount();
        Json::Value exchangeArray(Json::arrayValue);
        for (size_t i = 0; i < count; ++i) {
            Json::Value exchangeObj(Json::objectValue);
            ExchangeRate rateName = getExchangeRateName(i);
            exchangeObj["address"] = rateName.address;
            exchangeObj["index"] = rateName.index;
            exchangeObj["name"] = rateName.name;
            double rate = getExchangeRate(i);
            exchangeObj["rate"] = rate;
            exchangeArray.append(exchangeObj);
        }
        result["exchange"] = exchangeArray;
    }

    // Add KYC data
    if (isKYCTransaction()) {
        KYC kyc = getKYC();
        Json::Value kycObj(Json::objectValue);
        kycObj["address"] = kyc.getAddress();
        kycObj["country"] = kyc.getCountry();
        string name = kyc.getName();
        if (!name.empty()) {
            kycObj["name"] = name;
        }
        string hash = kyc.getHash();
        if (!hash.empty()) {
            kycObj["hash"] = hash;
        }
        kycObj["revoked"] = (!kyc.valid());
        result["kyc"] = kycObj;
    }

    return result;
}

DigiAsset DigiByteTransaction::getIssuedAsset() const {
    if (!isIssuance()) throw out_of_range("Not an issuance");
    return _newAsset;
}

