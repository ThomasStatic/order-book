#pragma once

#include <stdexcept>

namespace order_book {

    enum class Side {
        BUY,
        SELL
    };

    enum class ExecutionLifeCycle {
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
        ExecutionLifeCycle execution;

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
        ExecutionLifeCycle getExecutionLevel() const;
        void setExecutionLevel(ExecutionLifeCycle newLevel);
        bool orderIsActive() const;
    };

}  // namespace order_book