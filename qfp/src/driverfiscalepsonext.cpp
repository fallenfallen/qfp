/*
*
* Copyright (C)2013, Samuel Isuani <sisuani@gmail.com>
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* Redistributions of source code must retain the above copyright notice,
* this list of conditions and the following disclaimer.
*
* Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
*
* Neither the name of the project's author nor the names of its
* contributors may be used to endorse or promote products derived from
* this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
* HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
* TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
* PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#include "driverfiscalepsonext.h"
#include "packagefiscal.h"

#if LOGGER
#include "logger.h"
#else
#define log qDebug()
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>

int PackageEpsonExt::m_secuence = 0x81;

DriverFiscalEpsonExt::DriverFiscalEpsonExt(QObject *parent, SerialPort *m_serialPort)
    : QThread(parent), DriverFiscal(parent, m_serialPort)
{
    QString path = QDir::homePath() + QDir::separator() + ".config" + QDir::separator() + "Subway" + QDir::separator();
#if LOGGER
    Logger::instance()->init(path + "fiscal.txt");
#endif
    m_nak_count = 0;
    m_error = false;
    m_isinvoice = false;
    m_continue = true;
    clear();
}

void DriverFiscalEpsonExt::setModel(const FiscalPrinter::Model model)
{
    m_model = model;
}

void DriverFiscalEpsonExt::finish()
{
    m_continue = false;
    queue.clear();
    quit();

    while(isRunning()) {
        msleep(100);
        quit();
    }
}

void DriverFiscalEpsonExt::run()
{

    while(!queue.empty() && m_continue) {
        PackageEpsonExt *pkg = queue.first();
        m_serialPort->write(pkg->fiscalPackage());

        QByteArray ret = readData(pkg->cmd());
        if(!ret.isEmpty()) {
            if(ret.at(0) == PackageFiscal::NAK && m_nak_count <= 3) { // ! NAK
                m_nak_count++;
                qDebug() << "NAK";
                SleeperThread::msleep(100);
                continue;
            }

            m_nak_count = 0;

            if(pkg->cmd() == CMD_CLOSEFISCALRECEIPT_INVOICE ||
                    pkg->cmd() == CMD_CLOSEFISCALRECEIPT_TICKET || pkg->cmd() == CMD_CLOSEDNFH) {
                if(pkg->data()[0] == 'M') {
                    emit fiscalReceiptNumber(pkg->id(), getReceiptNumber(ret), 1);
                } else {
                    emit fiscalReceiptNumber(pkg->id(), getReceiptNumber(ret), 0);
                }
            }

            queue.pop_front();
            sendAck();
            delete pkg;

        } else {
            log << QString("FISCAL ERROR?");
            sendAck();
            queue.clear();
        }
    }
}

void DriverFiscalEpsonExt::sendAck()
{
    QByteArray ack;
    ack.append(PackageFiscal::ACK);
    m_serialPort->write(ack);
    QByteArray bufferBytes = m_serialPort->read(1);
#if LOGGER
    qDebug() << QString("SEND ACK") << bufferBytes;
#endif
}

int DriverFiscalEpsonExt::getReceiptNumber(const QByteArray &data)
{
    QByteArray tmp = data;
    tmp.remove(0, 14);
    for(int i = 0; i < tmp.size(); i++) {
        if(tmp.at(i) == PackageFiscal::ETX) {
            tmp.remove(i, tmp.size());
            break;
        }
    }

    log << QString("F. Num: %1").arg(tmp.trimmed().toInt());
    return tmp.trimmed().toInt();
}

bool DriverFiscalEpsonExt::getStatus(const QByteArray &data)
{
    QByteArray tmp = data;
    tmp.remove(0, 11);
    tmp.remove(5, tmp.size());

    return tmp == QByteArray("0000");
}

QByteArray DriverFiscalEpsonExt::readData(const int pkg_cmd)
{
    bool ok = false;
    int count_tw = 0;
    QByteArray bytes;

    do {
        if(m_serialPort->bytesAvailable() <= 0 && m_continue) {
            log << QString("NB tw");
            count_tw++;
            SleeperThread::msleep(100);
        }

        if(!m_continue)
            return "";

        QByteArray bufferBytes = m_serialPort->read(1);
        if(bufferBytes.at(0) == PackageFiscal::DC1 || bufferBytes.at(0) == PackageFiscal::DC2
                || bufferBytes.at(0) == PackageFiscal::DC3 || bufferBytes.at(0) == PackageFiscal::DC4
                || bufferBytes.at(0) == PackageFiscal::FNU || bufferBytes.at(0) == PackageFiscal::ACK) {
            SleeperThread::msleep(100);
            //count_tw -= 20;
            continue;
        } else if(bufferBytes.at(0) == PackageFiscal::NAK) {
            return bufferBytes;
        } else if(bufferBytes.at(0) == PackageFiscal::STX) {
            bufferBytes = m_serialPort->read(1);

            while (QString(bufferBytes.toHex()) == "80") {
                SleeperThread::msleep(200);
                count_tw++;

                bufferBytes = m_serialPort->read(1);
                QByteArray checkSumArray;
                int checksumCount = 0;
                while(checksumCount != 5 && m_continue) {
                    checkSumArray = m_serialPort->read(1);
                    count_tw++;
                    if(checkSumArray.isEmpty()) {
                        SleeperThread::msleep(100);
                    } else {
                        checksumCount++;
                    }

                    if(count_tw >= MAX_TW)
                        break;
                }
                //log << "END INTER";
                bufferBytes = m_serialPort->read(1);
            }

            bytes += PackageFiscal::STX;

            while(bufferBytes.at(0) != PackageFiscal::ETX) {

                count_tw++;
                if(bufferBytes.isEmpty()) {
                    SleeperThread::msleep(100);
                }

                bytes += bufferBytes;
                bufferBytes = m_serialPort->read(1);

                if(count_tw >= MAX_TW)
                    break;
            }

            bytes += PackageFiscal::ETX;

            QByteArray checkSumArray;
            int checksumCount = 0;
            while(checksumCount != 4 && m_continue) {
                checkSumArray = m_serialPort->read(1);
                count_tw++;
                if(checkSumArray.isEmpty()) {
                    SleeperThread::msleep(100);
                } else {
                    checksumCount++;
                    bytes += checkSumArray;
                }

                if(count_tw >= MAX_TW)
                    break;
            }


            ok = true;
            break;
        } else {
            bytes += bufferBytes;
        }
        count_tw++;


    } while(ok != true && count_tw <= MAX_TW && m_continue);

    //log << QString("COUNTER: %1 %2").arg(count_tw).arg(MAX_TW);

    ok = verifyResponse(bytes, pkg_cmd);


    if(!ok) {
        log << QString("ERROR READ: %1").arg(bytes.toHex().data());
        if(pkg_cmd == 42) {
            log << QString("SIGNAL ->> ENVIO STATUS ERROR");
            m_serialPort->readAll();
            emit fiscalStatus(false);
        }
    } else {
        log << QString("--> OK PKGV3: %1").arg(bytes.toHex().data());
    }

    return bytes;
}

bool DriverFiscalEpsonExt::verifyResponse(const QByteArray &bytes, const int pkg_cmd)
{
    if(bytes.at(0) != PackageFiscal::STX) {
        log << QString("NO STX %1").arg(bytes.toHex().data());
        if(pkg_cmd == 42) {
            log << QString("SIGNAL ->> ENVIO STATUS ERROR");
            emit fiscalStatus(false);
        }
        return false;
    }

    /*
    if(QChar(bytes.at(2)).unicode() != pkg_cmd) {
        log << QString("ERR - diff cmds: %1 %2").arg(QChar(bytes.at(2)).unicode()).arg(pkg_cmd);
        if(pkg_cmd == 42) {
            log << QString("SIGNAL ->> ENVIO STATUS ERROR");
            emit fiscalStatus(false);
        }

        return false;
    }
    */

    if(bytes.at(4) != PackageFiscal::FS) {
        log << QString("Error: diff FS");

        if(pkg_cmd == 42) {
            log << QString("SIGNAL ->> ENVIO STATUS ERROR");
            emit fiscalStatus(false);
        }
        return false;
    }

    if(bytes.at(bytes.size() - 5) != PackageFiscal::ETX) {
        log << QString("Error: ETX");

        if(pkg_cmd == 42) {
            log << QString("SIGNAL ->> ENVIO STATUS ERROR");
            emit fiscalStatus(false);
        }
        return false;
    }

    /*
    if(!checkSum(bytes)) {
        log << QString("Error: checksum");

        if(pkg_cmd == 42) {
            log << QString("SIGNAL ->> ENVIO STATUS ERROR");
            emit fiscalStatus(false);
        }
        return false;
    }
    */

    return true;
}

bool DriverFiscalEpsonExt::checkSum(const QString &data)
{
    bool ok;
    int checkSum = data.right(4).toInt(&ok, 16);
    if(!ok) {
        return false;
    }

    int ck_cmd = 0;
    for(int i = 0; i < data.size()-4; i++) {
        ck_cmd += QChar(data.at(i)).unicode();
    }


    return (checkSum == ck_cmd);
}

void DriverFiscalEpsonExt::statusRequest()
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_STATUS);
    QByteArray d;

    // CMD
    d.append(QByteArray::fromHex("0"));
    d.append(0x01);
    // DATA
    d.append(PackageFiscal::FS);
    d.append(QByteArray::fromHex("0"));
    d.append(QByteArray::fromHex("0"));

    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::dailyClose(const char type)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_DAILYCLOSE);

    QByteArray d;

    // CMD
    d.append(0x08);
    if (type == 'X') {
        d.append(0x1B);
        d.append(0x02);
        // DATA
        d.append(PackageFiscal::FS);
        d.append(0x0C);
        d.append(0x01);
    } else { // Z
        d.append(0x01);
        // DATA
        d.append(PackageFiscal::FS);
        d.append(0x0C);
        d.append(QByteArray::fromHex("0"));
    }

    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::dailyCloseByDate(const QDate &from, const QDate &to)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_DAILYCLOSEBYDATE);

    QByteArray d;
    d.append(from.toString("yyMMdd"));
    d.append(PackageFiscal::FS);
    d.append(to.toString("yyMMdd"));
    d.append(PackageFiscal::FS);
    d.append('T');
    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::dailyCloseByNumber(const int from, const int to)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_DAILYCLOSEBYNUMBER);

    QByteArray d;
    d.append(QString::number(from));
    d.append(PackageFiscal::FS);
    d.append(QString::number(to));
    d.append(PackageFiscal::FS);
    d.append('T');
    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::setCustomerData(const QString &name, const QString &cuit, const char tax_type,
        const QString &doc_type, const QString &address)
{
    m_name = name;
    m_cuit = cuit;
    if(tax_type == 'A')
        m_tax_type = 'N';
    else if(tax_type == 'C')
        m_tax_type = 'F';
    else
        m_tax_type = tax_type;


    if(doc_type.compare("C") == 0)
        m_doc_type = "CUIT";
    else if(doc_type.compare("3") == 0)
        m_doc_type = "PASAPO";
    else
        m_doc_type = "DNI";
    m_address = address;
}

void DriverFiscalEpsonExt::openFiscalReceipt(const char type)
{
    PackageEpsonExt *p = new PackageEpsonExt;

    QByteArray d;
    if(type == 'A' || type == 'B') {
        m_isinvoice = true;
        p->setCmd(CMD_OPENFISCALRECEIPT);


        d.append('T');
        d.append(PackageFiscal::FS);
        d.append('C');
        d.append(PackageFiscal::FS);
        d.append(type);
        d.append(PackageFiscal::FS);
        d.append('1');
        d.append(PackageFiscal::FS);
        d.append('F');
        d.append(PackageFiscal::FS);
        d.append(QString::number(12).toLatin1());
        d.append(PackageFiscal::FS);
        d.append('I');
        d.append(PackageFiscal::FS);
        d.append(m_tax_type);
        d.append(PackageFiscal::FS);
        d.append(m_name);
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append(m_doc_type);
        d.append(PackageFiscal::FS);
        d.append(m_cuit);
        d.append(PackageFiscal::FS);
        d.append('N');
        d.append(PackageFiscal::FS);
        d.append(m_address);
        d.append(PackageFiscal::FS);
        d.append(m_address1);
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append(m_refer);
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
    } else { // 'T'
        m_isinvoice = false;
        p->setCmd(CMD_OPENTICKET);
        // CMD
        d.append(0x0A);
        d.append(0x01);
        // DATA
        d.append(PackageFiscal::FS);
        d.append(QByteArray::fromHex("0"));
        d.append(QByteArray::fromHex("0"));
    }

    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::printFiscalText(const QString &text)
{
}

void DriverFiscalEpsonExt::printLineItem(const QString &description, const qreal quantity,
        const qreal price, const QString &tax, const char qualifier)
{

    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(m_isinvoice ? CMD_PRINTLINEITEM_INVOICE : CMD_PRINTLINEITEM_TICKET);

    QByteArray d;
    if (m_isinvoice) {
    } else {
        d.append(0x0A);
        d.append(0x1B);
        d.append(0x02);
        d.append(PackageFiscal::FS);
        d.append(QByteArray::fromHex("0"));
        d.append(0x10);
    }

    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(description.left(20).toLatin1());
    d.append(PackageFiscal::FS);
    d.append(QString::number(quantity * 10000, 'f', 0));
    d.append(PackageFiscal::FS);
    if(m_tax_type == 'I') {
        d.append(QString::number(price/(1+(tax.toDouble()/100)) * 100, 'f', 0));
    } else {
        d.append(QString::number(price * 100, 'f', 0));
    }
    d.append(PackageFiscal::FS);
    d.append(QString::number(tax.toDouble() * 100, 'f', 0));
    d.append(PackageFiscal::FS);
    //d.append(qualifier);
    d.append(PackageFiscal::FS);
    if(m_isinvoice) {
        d.append("0000");
        d.append(PackageFiscal::FS);
        d.append("00000000");
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append("0000");
        d.append(PackageFiscal::FS);
        d.append("00000000000000000");
    } else {
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(0x01);
        d.append(PackageFiscal::FS);
    }
    p->setData(d);

    queue.append(p);
    start();

}

void DriverFiscalEpsonExt::perceptions(const QString &desc, qreal tax_amount)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_PERCEPTIONS);

    QByteArray d;
    d.append(desc);
    d.append(PackageFiscal::FS);
    d.append('O');
    d.append(PackageFiscal::FS);
    d.append(QString::number(tax_amount * 100, 'f', 0));
    d.append(PackageFiscal::FS);
    d.append("0");
    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::subtotal(const char print)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_SUBTOTAL);

    QByteArray d;
    d.append(print);
    d.append(PackageFiscal::FS);
    d.append("Subtotal");
    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::totalTender(const QString &description, const qreal amount, const char type)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(m_isinvoice ? CMD_TOTALTENDER_INVOICE : CMD_TOTALTENDER_TICKET);

    QByteArray d;
    d.append(description);
    d.append(PackageFiscal::FS);
    d.append(QString::number(amount * 100, 'f', 0).rightJustified(9, '0'));
    d.append(PackageFiscal::FS);
    d.append(type);
    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::generalDiscount(const QString &description, const qreal amount, const char type)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(m_isinvoice ? CMD_PRINTLINEITEM_INVOICE : CMD_PRINTLINEITEM_TICKET);

    QByteArray d;
    d.append(description.left(20));
    d.append(PackageFiscal::FS);
    d.append("1000");
    d.append(PackageFiscal::FS);
    if(m_tax_type == 'I') {
        d.append(QString::number((amount/1.21) * 100, 'f', 0));
//	const qreal a = amount * 21 / 100;
 //       d.append(QString::number((amount) * 100, 'f', 0));
    } else {
        d.append(QString::number(amount * 100, 'f', 0));
    }
    d.append(PackageFiscal::FS);
    d.append("2100");
    d.append(PackageFiscal::FS);
    d.append(type == 'M' ? 'M' : 'R');
    d.append(PackageFiscal::FS);
    if(m_isinvoice) {
        d.append("0000");
        d.append(PackageFiscal::FS);
        d.append("00000000");
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append("");
        d.append(PackageFiscal::FS);
        d.append("0000");
        d.append(PackageFiscal::FS);
        d.append("00000000000000000");
    } else {
        d.append("0");
        d.append(PackageFiscal::FS);
        d.append("00000000");
        d.append(PackageFiscal::FS);
        d.append("00000000000000000");
    }
    p->setData(d);

    queue.append(p);
    start();

}

void DriverFiscalEpsonExt::closeFiscalReceipt(const char intype, const char type, const int id)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setId(id);
    p->setCmd(m_isinvoice ? CMD_CLOSEFISCALRECEIPT_INVOICE : CMD_CLOSEFISCALRECEIPT_TICKET);

    QByteArray d;
    if(m_isinvoice) {
        d.append(intype);
        d.append(PackageFiscal::FS);
        d.append(type);
        d.append(PackageFiscal::FS);
        d.append("0");
    } else {
        d.append(0x0A);
        d.append(0x1B);
        d.append(0x06);
        d.append(PackageFiscal::FS);
        d.append(QByteArray::fromHex("0"));
        d.append(0x01);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
        d.append(PackageFiscal::FS);
    }


    clear();

    p->setData(d);
    queue.append(p);
    start();

}

void DriverFiscalEpsonExt::openNonFiscalReceipt()
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_OPENNONFISCALRECEIPT);

    QByteArray d;
    // CMD
    d.append(0x0E);
    d.append(0x01);
    // DATA
    d.append(PackageFiscal::FS);
    d.append(QByteArray::fromHex("0"));
    d.append(QByteArray::fromHex("0"));

    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::printNonFiscalText(const QString &text)
{
    QString t = text;
    while(!t.isEmpty()) {
        PackageEpsonExt *p = new PackageEpsonExt;
        p->setCmd(CMD_PRINTNONTFISCALTEXT);

        QByteArray d;
        // CMD
        d.append(0x0E);
        d.append(0x1B);
        d.append(0x02);
        // DATA
        d.append(PackageFiscal::FS);
        d.append(QByteArray::fromHex("0"));
        d.append(QByteArray::fromHex("0"));
        d.append(PackageFiscal::FS);
        d.append(t.left(39).toLatin1());
        p->setData(d);

        t.remove(0, 39);

        queue.append(p);
    }

    start();
}

void DriverFiscalEpsonExt::closeNonFiscalReceipt()
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_CLOSENONFISCALRECEIPT);

    QByteArray d;
    // CMD
    d.append(0x0E);
    d.append(0x06);
    // DATA
    d.append(PackageFiscal::FS);
    d.append(QByteArray::fromHex("0"));
    d.append(0x01);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    d.append(PackageFiscal::FS);
    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::openDrawer()
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_OPENDRAWER);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::setHeaderTrailer(const QString &header, const QString &trailer)
{
}

void DriverFiscalEpsonExt::setEmbarkNumber(const int doc_num, const QString &description)
{
    m_refer = description;
}

void DriverFiscalEpsonExt::openDNFH(const char type, const char fix_value, const QString &doc_num)
{
    PackageEpsonExt *p = new PackageEpsonExt;

    QByteArray d;
    m_isinvoice = true;
    p->setCmd(CMD_OPENFISCALRECEIPT);
    d.append('M');
    d.append(PackageFiscal::FS);
    d.append('C');
    d.append(PackageFiscal::FS);
    d.append(type == 'R' ? 'A' : 'B');
    d.append(PackageFiscal::FS);
    d.append('1');
    d.append(PackageFiscal::FS);
    d.append('F');
    d.append(PackageFiscal::FS);
    d.append(QString::number(12).toLatin1());
    d.append(PackageFiscal::FS);
    d.append('I');
    d.append(PackageFiscal::FS);
    d.append(m_tax_type);
    d.append(PackageFiscal::FS);
    d.append(m_name);
    d.append(PackageFiscal::FS);
    d.append("");
    d.append(PackageFiscal::FS);
    d.append(m_doc_type);
    d.append(PackageFiscal::FS);
    d.append(m_cuit);
    d.append(PackageFiscal::FS);
    d.append('N');
    d.append(PackageFiscal::FS);
    d.append(m_address);
    d.append(PackageFiscal::FS);
    d.append(m_address1);
    d.append(PackageFiscal::FS);
    d.append("");
    d.append(PackageFiscal::FS);
    d.append(doc_num);
    d.append(PackageFiscal::FS);
    d.append("");
    d.append(PackageFiscal::FS);
    d.append('C');
    p->setData(d);

    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::printEmbarkItem(const QString &description, const qreal quantity)
{
}

void DriverFiscalEpsonExt::closeDNFH(const int id, const char f_type, const int copies)
{
    Q_UNUSED(f_type);
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setId(id);
    p->setCmd(CMD_CLOSEFISCALRECEIPT_INVOICE);

    QByteArray d;
    d.append('M');
    d.append(PackageFiscal::FS);
    d.append(m_tax_type == 'I' ? 'A' : 'B');
    d.append(PackageFiscal::FS);
    d.append("0");

    clear();

    p->setData(d);

    qDebug() << (p->data()[0] == 'M');
    queue.append(p);
    start();
}

void DriverFiscalEpsonExt::cancel()
{
}

void DriverFiscalEpsonExt::ack()
{
}

void DriverFiscalEpsonExt::setDateTime(const QDateTime &dateTime)
{
    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(CMD_SETDATETIME);

    QByteArray d;
    d.append(dateTime.toString("yyMMdd").toLatin1());
    d.append(PackageFiscal::FS);
    d.append(dateTime.toString("HHmmss").toLatin1());

    p->setData(d);
    queue.append(p);

    start();
}

void DriverFiscalEpsonExt::clear()
{
    m_name = "Consumidor Final";
    m_cuit = "0";
    m_tax_type = 'F';
    m_doc_type = "DNI";
    m_address = "-";
    m_address1 = "";
    m_refer = "-";
}

void DriverFiscalEpsonExt::receiptText(const QString &text)
{
}

void DriverFiscalEpsonExt::setFixedData(const QString &shop, const QString &phone)
{
    QByteArray d;

    PackageEpsonExt *pp = new PackageEpsonExt;
    pp->setCmd(0x5d);

    d.append("00011");
    d.append(PackageFiscal::FS);
    d.append("DEFENSA CONSUMIDOR " + phone);
    pp->setData(d);
    queue.append(pp);
    d.clear();

    PackageEpsonExt *p = new PackageEpsonExt;
    p->setCmd(0x5d);


    d.append("00012");
    d.append(PackageFiscal::FS);
    d.append("*************************************");
    p->setData(d);
    queue.append(p);
    d.clear();

    PackageEpsonExt *p1 = new PackageEpsonExt;
    p1->setCmd(0x5d);

    d.append("00013");
    d.append(PackageFiscal::FS);
    d.append("Entra con este ticket a:");
    p1->setData(d);
    queue.append(p1);
    d.clear();

    PackageEpsonExt *p2 = new PackageEpsonExt;
    p2->setCmd(0x5d);

    d.append("00014");
    d.append(PackageFiscal::FS);
    d.append("www.cuentaleasubway.com y llevate una");
    p2->setData(d);
    queue.append(p2);
    d.clear();

    PackageEpsonExt *p3 = new PackageEpsonExt;
    p3->setCmd(0x5d);

    d.append("00015");
    d.append(PackageFiscal::FS);
    d.append("COOKIE GRATIS en tu proxima compra.");
    p3->setData(d);
    queue.append(p3);
    d.clear();

    PackageEpsonExt *p4 = new PackageEpsonExt;
    p4->setCmd(0x5d);

    d.append("00016");
    d.append(PackageFiscal::FS);
    d.append("Restaurante ID: " + shop);
    p4->setData(d);
    queue.append(p4);
    d.clear();

    PackageEpsonExt *p5 = new PackageEpsonExt;
    p5->setCmd(0x5d);

    d.append("00017");
    d.append(PackageFiscal::FS);
    d.append("*************************************");
    p5->setData(d);
    queue.append(p5);
    d.clear();

    PackageEpsonExt *p6 = new PackageEpsonExt;
    p6->setCmd(0x5d);

    d.append("00018");
    d.append(PackageFiscal::FS);
    d.append(" ");
    p6->setData(d);
    queue.append(p6);
    d.clear();

    PackageEpsonExt *p7 = new PackageEpsonExt;
    p7->setCmd(0x5d);

    d.append("00019");
    d.append(PackageFiscal::FS);
    d.append(" ");
    p7->setData(d);
    queue.append(p7);
    d.clear();

    PackageEpsonExt *p8 = new PackageEpsonExt;
    p8->setCmd(0x5d);

    d.append("00020");
    d.append(PackageFiscal::FS);
    d.append(" ");
    p8->setData(d);
    queue.append(p8);
    d.clear();

    PackageEpsonExt *p9 = new PackageEpsonExt;
    p9->setCmd(0x5d);

    d.append("00021");
    d.append(PackageFiscal::FS);
    d.append(" ");
    p9->setData(d);
    queue.append(p9);
    d.clear();

    start();
}