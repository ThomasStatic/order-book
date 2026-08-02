#pragma once

#include <stdexcept>

namespace order_book {

    enum class Side {
        BUY,
        SELL
    };

    enum class FillStatus {
        NONE,
        PARTIAL,
        FILLED
    };

    class Order {
    private:
        unsigned int orderId;
        unsigned int sequenceNum;

        unsigned int priceTick;
        unsigned int initialQuant;
        unsigned int remainingQuant;

        Side side;
        FillStatus execution;

    public:
        Order(unsigned int id, unsigned int tick, unsigned int quant, unsigned int arrival, Side s);

        unsigned int getOrderId() const;
        unsigned int getSequenceNum() const;
        void decrementSequence();

        unsigned int getPriceTick() const;
        unsigned int getInitialQuantity() const;
        unsigned int getRemainingQuantity() const;
        unsigned int fillQuantity(unsigned int amt);

        Side getSide() const;
        FillStatus getExecutionLevel() const;
        void setExecutionLevel(FillStatus newLevel);
        bool orderIsActive() const;
    };

}  // namespace order_book